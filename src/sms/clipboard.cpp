/**
 * @file src/sms/clipboard.cpp
 * @brief Simpleminded Screens: lets a paired viewer read the host's clipboard text during a stream.
 */
// local includes
#include "clipboard.h"

// standard includes
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// platform includes
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

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
        {
          // XML 1.0 cannot carry control characters other than tab, LF and CR.
          std::string text;
          for (const char c : result.text) {
            if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n' || c == '\r') {
              text += c;
            }
          }
          tree.put("root.text", text);
        }
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

  namespace {
    /**
     * @brief The clipboard program for this desktop.
     */
    struct reader_t {
      std::vector<std::string> argv;  ///< Command and arguments.
      std::string install_hint;  ///< Shown when the command is missing.
      std::string wayland_display;  ///< Set for the child when Sunshine's own environment lacks it.
    };

    std::optional<reader_t> pick_reader() {
      std::string wayland = std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY") : "";
      if (wayland.empty()) {
        // A user service may start without the desktop's environment; the socket is still there.
        if (const char *runtime = std::getenv("XDG_RUNTIME_DIR")) {
          std::error_code ec;
          // Explicit loop: the range-for form can throw when advancing.
          for (std::filesystem::directory_iterator it(runtime, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.starts_with("wayland-") && !name.ends_with(".lock") && it->is_socket(ec)) {
              wayland = name;
              break;
            }
          }
        }
      }
      if (!wayland.empty()) {
        return reader_t {{"wl-paste", "--no-newline", "--type", "text"}, "Install wl-clipboard on the host", wayland};
      }
      if (std::getenv("DISPLAY")) {
        return reader_t {{"xclip", "-o", "-selection", "clipboard"}, "Install xclip on the host", {}};
      }
      return std::nullopt;
    }
  }  // namespace

  read_result read_clipboard() {
    const auto reader = pick_reader();
    if (!reader) {
      return {read_status::failed};
    }

    // Everything the child needs is built before fork: Sunshine is multithreaded, so the child may
    // only make async-signal-safe calls.
    std::vector<char *> argv;
    for (const auto &arg : reader->argv) {
      argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    std::vector<std::string> env_strings;
    for (char **e = environ; *e; ++e) {
      if (!std::string_view(*e).starts_with("WAYLAND_DISPLAY=")) {
        env_strings.emplace_back(*e);
      }
    }
    if (!reader->wayland_display.empty()) {
      env_strings.push_back("WAYLAND_DISPLAY=" + reader->wayland_display);
    }
    std::vector<char *> envp;
    for (auto &s : env_strings) {
      envp.push_back(s.data());
    }
    envp.push_back(nullptr);

    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) {
      return {read_status::failed};
    }
    const int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    const pid_t pid = fork();
    if (pid < 0) {
      close(out[0]);
      close(out[1]);
      if (devnull >= 0) {
        close(devnull);
      }
      return {read_status::failed};
    }
    if (pid == 0) {
      dup2(out[1], STDOUT_FILENO);
      if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
      }
      close_range(3, ~0U, 0);  // don't leak Sunshine's non-CLOEXEC sockets to the reader
      execvpe(argv[0], argv.data(), envp.data());
      _exit(127);
    }
    close(out[1]);
    if (devnull >= 0) {
      close(devnull);
    }

    std::string output;
    bool timed_out = false;
    bool truncated = false;
    const auto deadline = std::chrono::steady_clock::now() + clipboard_timeout;
    char buffer[65536];
    while (true) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (left <= 0) {
        timed_out = true;
        break;
      }
      pollfd ready {out[0], POLLIN, 0};
      const int n = poll(&ready, 1, static_cast<int>(left));
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        timed_out = true;
        break;
      }
      const ssize_t got = read(out[0], buffer, sizeof(buffer));
      if (got < 0 && errno == EINTR) {
        continue;
      }
      if (got <= 0) {
        break;  // the reader closed its output
      }
      output.append(buffer, static_cast<std::size_t>(got));
      if (output.size() > clipboard_limit) {
        truncated = true;
        break;
      }
    }
    close(out[0]);
    if (timed_out || truncated) {
      kill(pid, SIGKILL);
    }
    // A reader that closes stdout and then hangs must not outlive the deadline either.
    int status = 0;
    pid_t reaped = 0;
    while (reaped == 0 && std::chrono::steady_clock::now() < deadline) {
      reaped = waitpid(pid, &status, WNOHANG);
      if (reaped < 0 && errno == EINTR) {
        reaped = 0;
      }
      if (reaped == 0) {
        usleep(10000);
      }
    }
    if (reaped == 0) {
      kill(pid, SIGKILL);
      do {
        reaped = waitpid(pid, &status, 0);
      } while (reaped < 0 && errno == EINTR);
    }
    // -1 (e.g. ECHILD: another thread reaped the child) leaves status meaningless.
    const int exit_code = (reaped > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return result_from_run(true, timed_out, exit_code, std::move(output), truncated, reader->install_hint);
  }
}  // namespace sms
