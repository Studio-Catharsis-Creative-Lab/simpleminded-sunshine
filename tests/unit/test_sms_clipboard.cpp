/**
 * @file tests/unit/test_sms_clipboard.cpp
 * @brief Simpleminded Screens: the /clipboard reply and reader.
 */
#include "../tests_common.h"

#include <sstream>

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
