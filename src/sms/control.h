/**
 * @file src/sms/control.h
 * @brief Simpleminded Screens: one remote viewer drives at a time (the control baton).
 */
#pragma once

// standard includes
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// lib includes
#include <boost/property_tree/ptree.hpp>

namespace sms::control {
  using clock_t = std::chrono::steady_clock;

  /**
   * @brief Short, stable, public id for a seat: 12 hex chars of SHA-256 of its certificate.
   */
  std::string seat_id(const std::string &cert);

  /**
   * @brief One connected remote viewer.
   */
  struct seat_t {
    std::string cert;  ///< Paired client certificate (PEM). Never sent to viewers.
    std::string id;  ///< seat_id(cert).
    std::string name;  ///< Paired-device name.
    bool asked = false;  ///< Asked for control and has not got it yet.
    clock_t::time_point joined;  ///< When the seat connected.
  };

  /**
   * @brief Outcome of a viewer's action.
   */
  enum class result_e {
    ok,  ///< Applied.
    not_driver,  ///< Give from a seat that is not driving.
    not_owner,  ///< Take back from a seat that is not the owner.
    no_such_seat,  ///< Give to a seat that is gone.
    no_stream,  ///< No seat is connected.
  };

  /**
   * @brief The baton as one seat sees it.
   */
  struct view_t {
    bool driving = false;  ///< The asking seat drives.
    bool owner = false;  ///< The asking seat is the owner.
    std::string driver_name;  ///< Empty when nobody drives remotely.
    std::vector<seat_t> seats;  ///< Every other seat, in join order.
  };

  /**
   * @brief Seats, driver, owner and idle timer. Thread-safe.
   */
  class baton_t {
  public:
    explicit baton_t(std::function<clock_t::time_point()> now = clock_t::now);

    void join(const std::string &cert, const std::string &name);
    void leave(const std::string &cert);
    void set_owner_cert(const std::string &cert);  ///< The ticked owner's cert, or empty.
    void set_idle_limit(std::chrono::minutes limit);

    bool may_input(const std::string &cert);  ///< True for the driver; refreshes its idle clock.
    result_e request(const std::string &cert);
    result_e give(const std::string &cert, const std::string &to_id);
    result_e takeback(const std::string &cert);

    void host_takeback();  ///< Tray: nobody drives remotely until someone requests.
    void host_give(const std::string &to_id);  ///< Tray: give to a seat.
    void host_release();  ///< Tray: control is free, the next request gets it.

    view_t view(const std::string &cert);
    std::vector<seat_t> asked_seats();
    bool remote_driving();

    std::function<void(const std::string &old_driver_cert)> on_driver_change;  ///< Release that seat's held input.
    std::function<void(const std::string &name)> on_request;  ///< Tell the host computer.

  private:
    void expire_idle_locked();
    void set_driver_locked(const std::string &cert);
    std::string owner_cert_locked() const;
    seat_t *find_by_id_locked(const std::string &id);
    seat_t *find_locked(const std::string &cert);

    std::function<clock_t::time_point()> now_;
    std::mutex mutex_;
    std::vector<seat_t> seats_;
    std::string driver_;  ///< Cert of the driving seat, or empty.
    bool host_hold_ = false;  ///< Tray took control; a lone seat does not drive by itself.
    std::string ticked_owner_;
    std::chrono::minutes idle_limit_ {5};
    clock_t::time_point last_input_;
  };

  /**
   * @brief The process-wide baton.
   */
  baton_t &baton();

  /**
   * @brief The /control reply: state on ok, status_code and status_message otherwise.
   */
  boost::property_tree::ptree control_reply(result_e result, const view_t &view);
}  // namespace sms::control
