/**
 * @file src/sms/clipboard.h
 * @brief Simpleminded Screens: lets a paired viewer read the host's clipboard text during a stream.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <string>

// lib includes
#include <boost/property_tree/ptree.hpp>

namespace sms {
  constexpr std::size_t clipboard_limit = 1024 * 1024;  ///< Text beyond this is refused (413).
  constexpr auto clipboard_timeout = std::chrono::seconds(2);  ///< The reader is killed after this.
  constexpr const char *features = "clipboard";  ///< Advertised in serverinfo as SmsFeatures.

  /**
   * @brief How a clipboard read ended.
   */
  enum class read_status {
    ok,  ///< `text` holds the clipboard.
    empty,  ///< Nothing, or nothing textual, on the clipboard.
    too_large,  ///< Over clipboard_limit.
    no_reader,  ///< wl-paste / xclip is not installed.
    failed,  ///< The reader failed or timed out, or no desktop was found.
    not_streaming,  ///< Refused: no stream is running.
  };

  /**
   * @brief A clipboard read.
   */
  struct read_result {
    read_status status = read_status::failed;  ///< How it ended.
    std::string text;  ///< The clipboard, when status is ok.
    std::string install_hint;  ///< What to install, when status is no_reader.
  };

  /**
   * @brief Turns how the reader process ended into a result. Pure, so it can be tested.
   */
  read_result result_from_run(bool started, bool timed_out, int exit_code, std::string output, bool truncated, const std::string &install_hint);

  /**
   * @brief Reads the clipboard with wl-paste (Wayland) or xclip (X11). Blocks up to clipboard_timeout.
   */
  read_result read_clipboard();

  /**
   * @brief The /clipboard reply, in the XML shape of Sunshine's other replies.
   */
  boost::property_tree::ptree clipboard_reply(const read_result &result);
}  // namespace sms
