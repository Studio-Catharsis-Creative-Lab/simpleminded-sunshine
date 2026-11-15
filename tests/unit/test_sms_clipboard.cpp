/**
 * @file tests/unit/test_sms_clipboard.cpp
 * @brief Simpleminded Screens: the /clipboard reply and reader.
 */
#include "../tests_common.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include <unistd.h>

#include <boost/property_tree/xml_parser.hpp>

#include <src/sms/clipboard.h>

namespace {
  std::string xml_of(const sms::read_result &result) {
    std::ostringstream out;
    boost::property_tree::write_xml(out, sms::clipboard_reply(result));
    return out.str();
  }
}  // namespace

TEST(SmsClipboard, TextIsSentBackEscaped) {
  const auto tree = sms::clipboard_reply({sms::read_status::ok, "a <b> & \"c\"\nline 2", {}});
  EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 200);
  EXPECT_EQ(tree.get<std::string>("root.text"), "a <b> & \"c\"\nline 2");
  EXPECT_NE(xml_of({sms::read_status::ok, "a <b>", {}}).find("a &lt;b&gt;"), std::string::npos);
}

TEST(SmsClipboard, EachFailureHasItsStatusAndMessage) {
  auto check = [](sms::read_result result, int code, const std::string &message) {
    const auto tree = sms::clipboard_reply(result);
    EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), code);
    EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), message);
    EXPECT_FALSE(tree.get_child_optional("root.text"));
  };
  check({sms::read_status::not_streaming, {}, {}}, 403, "Clipboard is shared only during a stream");
  check({sms::read_status::empty, {}, {}}, 404, "The host's clipboard has no text");
  check({sms::read_status::too_large, {}, {}}, 413, "The host's clipboard is too large (over 1 MB)");
  check({sms::read_status::no_reader, {}, "Install xclip on the host"}, 501, "Install xclip on the host");
  check({sms::read_status::failed, {}, {}}, 500, "Could not read the host's clipboard");
}

TEST(SmsClipboard, RunOutcomesMapToStatuses) {
  const std::string hint = "Install wl-clipboard on the host";
  EXPECT_EQ(sms::result_from_run(false, false, 0, "", false, hint).status, sms::read_status::no_reader);
  EXPECT_EQ(sms::result_from_run(true, false, 127, "", false, hint).status, sms::read_status::no_reader);
  EXPECT_EQ(sms::result_from_run(true, false, 127, "", false, hint).install_hint, hint);
  EXPECT_EQ(sms::result_from_run(true, true, -1, "partial", false, hint).status, sms::read_status::failed);
  EXPECT_EQ(sms::result_from_run(true, false, 0, "x", true, hint).status, sms::read_status::too_large);
  // wl-paste and xclip exit 1 with nothing on stdout when the clipboard holds no text.
  EXPECT_EQ(sms::result_from_run(true, false, 1, "", false, hint).status, sms::read_status::empty);
  EXPECT_EQ(sms::result_from_run(true, false, 0, "", false, hint).status, sms::read_status::empty);
  EXPECT_EQ(sms::result_from_run(true, false, 2, "junk", false, hint).status, sms::read_status::failed);
  const auto ok = sms::result_from_run(true, false, 0, "hello", false, hint);
  EXPECT_EQ(ok.status, sms::read_status::ok);
  EXPECT_EQ(ok.text, "hello");
}

namespace {
  /**
   * @brief A fake wl-paste on PATH for one test, with WAYLAND_DISPLAY set. Restores both after.
   */
  class FakeReader {
  public:
    explicit FakeReader(const std::string &script) {
      dir_ = std::filesystem::temp_directory_path() / ("sms-clip-" + std::to_string(::getpid()));
      std::filesystem::create_directories(dir_);
      const auto path = dir_ / "wl-paste";
      std::ofstream(path) << "#!/bin/sh\n" << script << "\n";
      std::filesystem::permissions(path, std::filesystem::perms::owner_all);
      old_path_ = std::getenv("PATH") ? std::getenv("PATH") : "";
      const char *wayland = std::getenv("WAYLAND_DISPLAY");
      old_wayland_ = wayland ? std::optional<std::string>(wayland) : std::nullopt;
      ::setenv("PATH", (dir_.string() + ":" + old_path_).c_str(), 1);
      ::setenv("WAYLAND_DISPLAY", "wayland-test", 1);
    }

    ~FakeReader() {
      ::setenv("PATH", old_path_.c_str(), 1);
      if (old_wayland_) {
        ::setenv("WAYLAND_DISPLAY", old_wayland_->c_str(), 1);
      } else {
        ::unsetenv("WAYLAND_DISPLAY");
      }
      std::filesystem::remove_all(dir_);
    }

  private:
    std::filesystem::path dir_;
    std::string old_path_;
    std::optional<std::string> old_wayland_;
  };
}  // namespace

TEST(SmsClipboard, ReaderOutputComesBack) {
  FakeReader fake("printf 'from the host'");
  const auto result = sms::read_clipboard();
  EXPECT_EQ(result.status, sms::read_status::ok);
  EXPECT_EQ(result.text, "from the host");
}

TEST(SmsClipboard, EmptyClipboardIsEmpty) {
  FakeReader fake("echo 'Nothing is copied' >&2; exit 1");
  EXPECT_EQ(sms::read_clipboard().status, sms::read_status::empty);
}

TEST(SmsClipboard, OverOneMegabyteIsRefused) {
  FakeReader fake("head -c 1100000 /dev/zero | tr '\\0' 'a'");
  EXPECT_EQ(sms::read_clipboard().status, sms::read_status::too_large);
}

TEST(SmsClipboard, StuckReaderIsKilledAfterTwoSeconds) {
  FakeReader fake("exec sleep 10");
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(sms::read_clipboard().status, sms::read_status::failed);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(4));
}

TEST(SmsClipboard, ReaderThatClosesOutputThenHangsIsKilled) {
  FakeReader fake("exec 1>&-; sleep 10");
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(sms::read_clipboard().status, sms::read_status::failed);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(4));
}

TEST(SmsClipboard, ControlCharactersAreDroppedFromText) {
  const auto tree = sms::clipboard_reply({sms::read_status::ok, "a\x01" "b\x1b" "[0mc\td", {}});
  EXPECT_EQ(tree.get<std::string>("root.text"), "ab[0mc\td");
}
