/**
 * @file tests/unit/test_nvhttp_https.cpp
 * @brief Verify HTTPS callbacks retain the guard protecting server lifetime.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <future>
#include <thread>

// lib includes
#include <boost/asio/ssl.hpp>

// local includes
#include <src/crypto.h>
#include <src/nvhttp.h>

namespace {
  using namespace std::chrono_literals;
  using boost::asio::ip::tcp;

  /**
   * @brief Run real HTTPS callbacks against an isolated loopback listener.
   */
  struct HttpsHandlerLifetimeTest: BaseTest {
    /**
     * @brief Create credentials and start the production HTTPS accept handler.
     */
    void SetUp() override {
      BaseTest::SetUp();
      const auto credentials = crypto::gen_creds("Sunshine HTTPS lifetime test", 2048);
      certificate_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_lifetime_cert.pem";
      private_key_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_lifetime_key.pem";
      std::ofstream {certificate_file} << credentials.x509;
      std::ofstream {private_key_file} << credentials.pkey;

      server = nvhttp::test_support::make_https_server(certificate_file.string(), private_key_file.string(), handler_scope);
      server->io_service = std::make_shared<boost::asio::io_context>();
      server->config.address = "127.0.0.1";
      server->config.port = 0;
      server->start([this](unsigned short assigned_port) {
        port = assigned_port;
      });
      server->io_service->poll();
      ASSERT_NE(port, 0);
    }

    /**
     * @brief Drain canceled callbacks and remove the generated credentials.
     */
    void TearDown() override {
      if (server) {
        server->stop();
        server->io_service->restart();
        server->io_service->poll();
        server.reset();
      }
      if (stop_thread.joinable()) {
        stop_thread.join();
      }
      std::error_code cleanup_error;
      std::filesystem::remove(certificate_file, cleanup_error);
      std::filesystem::remove(private_key_file, cleanup_error);
      BaseTest::TearDown();
    }

    /**
     * @brief Check that stopping the scope runner waits until the error callback exits.
     *
     * @param callback_error Error delivered by the real accept or TLS handshake operation.
     */
    void check_handler_guard(const boost::system::error_code &callback_error) {
      EXPECT_TRUE(callback_error);
      ++error_callbacks;
      std::promise<void> stopping;
      auto stopping_future = stopping.get_future();
      std::promise<void> stopped;
      stopped_future = stopped.get_future();
      stop_thread = std::jthread([this, stopping = std::move(stopping), stopped = std::move(stopped)]() mutable {
        stopping.set_value();
        handler_scope->stop();
        stopped.set_value();
      });
      stopping_future.wait();
      EXPECT_EQ(stopped_future.wait_for(50ms), std::future_status::timeout);
    }

    std::unique_ptr<SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>> server;  ///< Server running production accept and handshake callbacks.
    std::shared_ptr<SimpleWeb::ScopeRunner> handler_scope;  ///< Guard used by the server destructor.
    std::jthread stop_thread;  ///< Concurrent attempt to stop the handler scope.
    std::future<void> stopped_future;  ///< Signals that all callback guards have been released.
    unsigned short port = 0;  ///< Assigned loopback port.
    int error_callbacks = 0;  ///< Number of errors delivered while testing a handler.
    std::filesystem::path certificate_file;  ///< Generated server certificate.
    std::filesystem::path private_key_file;  ///< Generated server private key.
  };

  /**
   * @brief Canceling a pending accept holds the server guard through its error callback.
   */
  TEST_F(HttpsHandlerLifetimeTest, AcceptErrorRetainsGuardUntilCallbackReturns) {
    server->on_error = [this](auto, const auto &callback_error) {
      check_handler_guard(callback_error);
    };
    server->stop();
    server->io_service->run_for(2s);
    ASSERT_EQ(error_callbacks, 1);
    ASSERT_TRUE(stopped_future.valid());
    EXPECT_EQ(stopped_future.wait_for(2s), std::future_status::ready);
  }

  /**
   * @brief A failed TLS handshake holds the server guard through its error callback.
   */
  TEST_F(HttpsHandlerLifetimeTest, HandshakeErrorRetainsGuardUntilCallbackReturns) {
    server->on_error = [this](auto, const auto &callback_error) {
      check_handler_guard(callback_error);
    };
    boost::asio::io_context client_io;
    tcp::socket client {client_io};
    client.connect({boost::asio::ip::address_v4::loopback(), port});
    client.close();
    server->io_service->run_for(2s);
    ASSERT_EQ(error_callbacks, 1);
    ASSERT_TRUE(stopped_future.valid());
    EXPECT_EQ(stopped_future.wait_for(2s), std::future_status::ready);
  }

  /**
   * @brief A stopped server scope prevents a canceled accept from dispatching an error.
   */
  TEST_F(HttpsHandlerLifetimeTest, StoppedScopeSkipsAcceptCallback) {
    server->on_error = [this](auto, const auto &) {
      ++error_callbacks;
    };
    handler_scope->stop();
    server->stop();
    server->io_service->run_for(2s);
    EXPECT_EQ(error_callbacks, 0);
  }

  /**
   * @brief A stopped server scope prevents an outstanding handshake from dispatching an error.
   */
  TEST_F(HttpsHandlerLifetimeTest, StoppedScopeSkipsHandshakeCallback) {
    server->on_error = [this](auto, const auto &) {
      ++error_callbacks;
    };
    boost::asio::io_context client_io;
    tcp::socket client {client_io};
    client.connect({boost::asio::ip::address_v4::loopback(), port});
    server->io_service->poll();
    handler_scope->stop();
    client.close();
    server->io_service->run_for(2s);
    EXPECT_EQ(error_callbacks, 0);
  }
  /**
   * @brief Simpleminded Screens: each connection's request is named by its own verified certificate.
   * @details Two clients finish their handshakes before either asks; the last handshake is the other client.
   */
  TEST(HttpsVerifiedClientCert, EachConnectionKeepsItsOwnCertificate) {
    const auto server_creds = crypto::gen_creds("Sunshine HTTPS identity test", 2048);
    const auto cert_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_identity_cert.pem";
    const auto key_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_identity_key.pem";
    std::ofstream {cert_file} << server_creds.x509;
    std::ofstream {key_file} << server_creds.pkey;

    std::shared_ptr<SimpleWeb::ScopeRunner> scope;
    auto server = nvhttp::test_support::make_https_server(cert_file.string(), key_file.string(), scope);
    std::atomic<int> handshakes = 0;
    nvhttp::test_support::set_https_verify(*server, [&handshakes](SSL *) {
      ++handshakes;
      return 1;
    });
    server->resource["^/who$"]["GET"] = [](auto response, auto request) {
      response->write(nvhttp::verified_client_cert(request));
      response->close_connection_after_response = true;
    };
    server->io_service = std::make_shared<boost::asio::io_context>();
    server->config.address = "127.0.0.1";
    server->config.port = 0;
    unsigned short port = 0;
    server->start([&port](unsigned short assigned) {
      port = assigned;
    });
    std::jthread io([&server] {
      server->io_service->run_for(10s);
    });
    while (port == 0) {
      std::this_thread::sleep_for(1ms);
    }

    struct client_t {
      crypto::creds_t creds;
      boost::asio::ssl::context context {boost::asio::ssl::context::tls_client};
      std::unique_ptr<boost::asio::ssl::stream<tcp::socket>> stream;
    };
    boost::asio::io_context client_io;
    std::array<client_t, 2> clients {client_t {crypto::gen_creds("Viewer A", 2048)}, client_t {crypto::gen_creds("Viewer B", 2048)}};
    for (auto &c : clients) {
      c.context.use_certificate(boost::asio::buffer(c.creds.x509), boost::asio::ssl::context::pem);
      c.context.use_private_key(boost::asio::buffer(c.creds.pkey), boost::asio::ssl::context::pem);
      c.stream = std::make_unique<boost::asio::ssl::stream<tcp::socket>>(client_io, c.context);
      c.stream->lowest_layer().connect({boost::asio::ip::address_v4::loopback(), port});
      c.stream->handshake(boost::asio::ssl::stream_base::client);
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (handshakes < 2 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(handshakes, 2);

    for (auto &c : clients) {
      boost::asio::write(*c.stream, boost::asio::buffer("GET /who HTTP/1.1\r\nHost: x\r\n\r\n"s));
      std::string reply;
      boost::system::error_code ec;
      std::array<char, 4096> chunk {};
      while (true) {
        const auto n = c.stream->read_some(boost::asio::buffer(chunk), ec);
        reply.append(chunk.data(), n);
        if (ec) {
          break;
        }
      }
      c.stream->shutdown(ec);  // the server's TLS shutdown waits for this close_notify
      c.stream->lowest_layer().close(ec);
      auto x509 = crypto::x509(c.creds.x509);
      EXPECT_NE(reply.find(crypto::pem(x509)), std::string::npos) << reply;
    }

    server->stop();
    io.join();
    std::error_code cleanup_error;
    std::filesystem::remove(cert_file, cleanup_error);
    std::filesystem::remove(key_file, cleanup_error);
  }
}  // namespace
