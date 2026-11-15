/**
 * @file src/sms/control.cpp
 * @brief Simpleminded Screens: one remote viewer drives at a time (the control baton).
 */
// local includes
#include "control.h"

// standard includes
#include <algorithm>
#include <cstdio>
#include <utility>

// lib includes
#include <openssl/sha.h>

namespace sms::control {
  std::string seat_id(const std::string &cert) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(cert.data()), cert.size(), digest);
    char hex[13];
    for (int i = 0; i < 6; ++i) {
      std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(hex, 12);
  }

  baton_t::baton_t(std::function<steady_clock_t::time_point()> now):
      now_(std::move(now)) {
  }

  seat_t *baton_t::find_locked(const std::string &cert) {
    auto it = std::find_if(seats_.begin(), seats_.end(), [&](const seat_t &s) {
      return s.cert == cert;
    });
    return it == seats_.end() ? nullptr : &*it;
  }

  seat_t *baton_t::find_by_id_locked(const std::string &id) {
    auto it = std::find_if(seats_.begin(), seats_.end(), [&](const seat_t &s) {
      return s.id == id;
    });
    return it == seats_.end() ? nullptr : &*it;
  }

  std::string baton_t::owner_cert_locked() const {
    if (!ticked_owner_.empty() && std::any_of(seats_.begin(), seats_.end(), [&](const seat_t &s) {
          return s.cert == ticked_owner_;
        })) {
      return ticked_owner_;
    }
    return seats_.empty() ? std::string {} : seats_.front().cert;  // seats_ is in join order
  }

  std::string baton_t::set_driver_locked(const std::string &cert) {
    if (driver_ == cert) {
      return {};
    }
    std::string old = driver_;
    dirty_ = true;
    driver_ = cert;
    last_input_ = now_();
    if (auto *seat = find_locked(cert)) {
      seat->asked = false;
    }
    return old;
  }

  std::string baton_t::expire_idle_locked() {
    if (!driver_.empty() && seats_.size() > 1 && now_() - last_input_ >= idle_limit_) {
      return set_driver_locked({});
    }
    return {};
  }

  void baton_t::notify_change(const std::string &old) {
    if (!old.empty() && on_driver_change) {
      on_driver_change(old);
    }
    bool changed;
    {
      std::lock_guard lock(mutex_);
      changed = std::exchange(dirty_, false);
    }
    if (changed && on_change) {
      on_change();
    }
  }

  void baton_t::join(const std::string &cert, const std::string &name) {
    {
      std::lock_guard lock(mutex_);
      if (auto *seat = find_locked(cert)) {
        ++seat->sessions;  // a reconnect while the old session still runs
        return;
      }
      dirty_ = true;
      seats_.push_back({cert, seat_id(cert), name, false, now_(), 1});
      if (seats_.size() == 1 && !host_hold_) {
        set_driver_locked(cert);  // nobody was driving: nothing to release
      } else if (seats_.size() == 2) {
        last_input_ = now_();  // an idle lone driver must not be dropped the instant someone joins
      }
    }
    notify_change({});
  }

  void baton_t::leave(const std::string &cert) {
    {
      std::lock_guard lock(mutex_);
      leave_locked(cert);
    }
    notify_change({});
  }

  void baton_t::leave_locked(const std::string &cert) {
    auto *seat = find_locked(cert);
    if (seat && --seat->sessions > 0) {
      return;  // another session of this cert is still live
    }
    dirty_ = true;
    std::erase_if(seats_, [&](const seat_t &s) {
      return s.cert == cert;
    });
    if (driver_ == cert) {
      driver_.clear();  // gone: nothing of its to release
    }
    if (seats_.empty()) {
      host_hold_ = false;
    }
  }

  void baton_t::set_owner_cert(const std::string &cert) {
    std::lock_guard lock(mutex_);
    ticked_owner_ = cert;
  }

  void baton_t::set_idle_limit(std::chrono::minutes limit) {
    std::lock_guard lock(mutex_);
    idle_limit_ = limit;
  }

  bool baton_t::may_input(const std::string &cert) {
    std::string old;
    bool driving = false;
    {
      std::lock_guard lock(mutex_);
      old = expire_idle_locked();
      if (!cert.empty() && find_locked(cert)) {
        if (driver_.empty() && !host_hold_ && seats_.size() == 1) {
          set_driver_locked(cert);  // alone: drives as with stock Sunshine
        }
        if (driver_ == cert) {
          last_input_ = now_();
          driving = true;
        }
      }
    }
    notify_change(old);
    return driving;
  }

  result_e baton_t::request(const std::string &cert) {
    std::string old;
    std::string name;
    result_e result = result_e::ok;
    {
      std::lock_guard lock(mutex_);
      old = expire_idle_locked();
      auto *seat = find_locked(cert);
      if (!seat) {
        result = result_e::no_stream;
      } else if (driver_ == cert) {
        // already driving
      } else if (driver_.empty()) {
        host_hold_ = false;
        set_driver_locked(cert);
      } else if (!seat->asked) {
        seat->asked = true;
        name = seat->name;
      }
    }
    notify_change(old);
    if (!name.empty() && on_request) {
      on_request(name);
    }
    return result;
  }

  result_e baton_t::give(const std::string &cert, const std::string &to_id) {
    std::string old;
    result_e result = result_e::ok;
    {
      std::lock_guard lock(mutex_);
      old = expire_idle_locked();
      if (cert.empty() || driver_ != cert || !find_locked(cert)) {
        result = result_e::not_driver;
      } else if (auto *to = find_by_id_locked(to_id)) {
        old = set_driver_locked(to->cert);
      } else {
        result = result_e::no_such_seat;
      }
    }
    notify_change(old);
    return result;
  }

  result_e baton_t::takeback(const std::string &cert) {
    std::string old;
    result_e result = result_e::ok;
    {
      std::lock_guard lock(mutex_);
      if (!find_locked(cert)) {
        return result_e::no_stream;
      }
      if (owner_cert_locked() != cert) {
        return result_e::not_owner;
      }
      host_hold_ = false;
      old = set_driver_locked(cert);
    }
    notify_change(old);
    return result;
  }

  void baton_t::host_takeback() {
    std::string old;
    {
      std::lock_guard lock(mutex_);
      if (seats_.empty()) {
        return;  // no stream: a hold would outlive nothing and block the next first seat
      }
      host_hold_ = true;
      old = set_driver_locked({});
    }
    notify_change(old);
  }

  void baton_t::host_give(const std::string &to_id) {
    std::string old;
    {
      std::lock_guard lock(mutex_);
      if (auto *to = find_by_id_locked(to_id)) {
        host_hold_ = false;
        old = set_driver_locked(to->cert);
      }
    }
    notify_change(old);
  }

  void baton_t::host_release() {
    std::string old;
    {
      std::lock_guard lock(mutex_);
      host_hold_ = false;
      old = set_driver_locked({});
    }
    notify_change(old);
  }

  view_t baton_t::view(const std::string &cert) {
    std::string old;
    view_t v;
    {
      std::lock_guard lock(mutex_);
      old = expire_idle_locked();
      v.driving = !driver_.empty() && driver_ == cert;
      v.owner = owner_cert_locked() == cert;
      if (auto *d = find_locked(driver_)) {
        v.driver_name = d->name;
        v.driver_id = d->id;
      }
      for (const auto &s : seats_) {
        if (s.cert != cert) {
          v.seats.push_back(s);
        }
      }
    }
    notify_change(old);
    return v;
  }

  std::vector<seat_t> baton_t::asked_seats() {
    std::lock_guard lock(mutex_);
    std::vector<seat_t> out;
    std::copy_if(seats_.begin(), seats_.end(), std::back_inserter(out), [](const seat_t &s) {
      return s.asked;
    });
    return out;
  }

  bool baton_t::seats_connected() {
    std::lock_guard lock(mutex_);
    return !seats_.empty();
  }

  bool baton_t::remote_driving() {
    std::string old;
    bool driving;
    {
      std::lock_guard lock(mutex_);
      old = expire_idle_locked();
      driving = !driver_.empty();
    }
    notify_change(old);
    return driving;
  }

  baton_t &baton() {
    static baton_t instance;
    return instance;
  }

  boost::property_tree::ptree control_reply(result_e result, const view_t &view) {
    boost::property_tree::ptree tree;
    auto fail = [&tree](int code, const char *message) {
      tree.put("root.<xmlattr>.status_code", code);
      tree.put("root.<xmlattr>.status_message", message);
    };
    switch (result) {
      case result_e::not_driver:
        fail(403, "Only the driver can give control");
        return tree;
      case result_e::not_owner:
        fail(403, "Only the owner can take control back");
        return tree;
      case result_e::no_such_seat:
        fail(404, "That viewer is no longer connected");
        return tree;
      case result_e::no_stream:
        fail(409, "No stream is running");
        return tree;
      case result_e::ok:
        break;
    }
    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.me", view.driving ? "driver" : "watcher");
    tree.put("root.owner", view.owner ? 1 : 0);
    tree.put("root.driver", view.driver_name);
    tree.put("root.driver_id", view.driver_id);
    for (const auto &s : view.seats) {
      boost::property_tree::ptree seat;
      seat.put("id", s.id);
      seat.put("name", s.name);
      seat.put("asked", s.asked ? 1 : 0);
      tree.add_child("root.seat", seat);
    }
    return tree;
  }
}  // namespace sms::control
