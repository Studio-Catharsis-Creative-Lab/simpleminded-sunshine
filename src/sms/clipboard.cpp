/**
 * @file src/sms/clipboard.cpp
 * @brief Simpleminded Screens: lets a paired viewer read the host's clipboard text during a stream.
 */
// local includes
#include "clipboard.h"

namespace sms {
  read_result result_from_run(bool started, bool timed_out, int exit_code, std::string output, bool truncated, const std::string &install_hint) {
    if (!started || exit_code == 127) {  // 127: exec found no such program
      return {read_status::no_reader, {}, install_hint};
    }
    if (timed_out) {
      return {read_status::failed};
    }
    if (truncated) {
      return {read_status::too_large};
    }
    if (output.empty() && (exit_code == 0 || exit_code == 1)) {
      return {read_status::empty};
    }
    if (exit_code != 0) {
      return {read_status::failed};
    }
    return {read_status::ok, std::move(output)};
  }

  boost::property_tree::ptree clipboard_reply(const read_result &result) {
    boost::property_tree::ptree tree;
    auto fail = [&tree](int code, const std::string &message) {
      tree.put("root.<xmlattr>.status_code", code);
      tree.put("root.<xmlattr>.status_message", message);
    };
    switch (result.status) {
      case read_status::ok:
        tree.put("root.<xmlattr>.status_code", 200);
        tree.put("root.text", result.text);
        break;
      case read_status::not_streaming:
        fail(403, "Clipboard is shared only during a stream");
        break;
      case read_status::empty:
        fail(404, "The host's clipboard has no text");
        break;
      case read_status::too_large:
        fail(413, "The host's clipboard is too large (over 1 MB)");
        break;
      case read_status::no_reader:
        fail(501, result.install_hint);
        break;
      case read_status::failed:
        fail(500, "Could not read the host's clipboard");
        break;
    }
    return tree;
  }
}  // namespace sms

namespace sms {
  read_result read_clipboard() {
    return {read_status::failed};
  }
}  // namespace sms
