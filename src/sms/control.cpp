/**
 * @file src/sms/control.cpp
 * @brief Simpleminded Screens: one remote viewer drives at a time (the control baton).
 */
// local includes
#include "control.h"

// standard includes
#include <algorithm>
#include <cstdio>

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

  baton_t::baton_t(std::function<clock_t::time_point()> now):
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

  void baton_t::set_driver_locked(const std::string &cert) {
    if (driver_ == cert) {
      return;
    }
    const std::string old = driver_;
    driver_ = cert;
    last_input_ = now_();
    if (auto *seat = find_locked(cert)) {
      seat->asked = false;
    }
    if (!old.empty() && on_driver_change) {
      on_driver_change(old);
    }
  }

  void baton_t::expire_idle_locked() {
    if (!driver_.empty() && seats_.size() > 1 && now_() - last_input_ >= idle_limit_) {
      set_driver_locked({});
    }
  }

  void baton_t::join(const std::string &cert, const std::string &name) {
    std::lock_guard lock(mutex_);
    if (find_locked(cert)) {
      return;
    }
    seats_.push_back({cert, seat_id(cert), name, false, now_()});
    if (seats_.size() == 1 && !host_hold_) {
      set_driver_locked(cert);
    }
  }

  void baton_t::leave(const std::string &cert) {
    std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
    expire_idle_locked();
    if (driver_.empty() && !host_hold_ && seats_.size() == 1 && seats_.front().cert == cert) {
      set_driver_locked(cert);  // alone: drives as with stock Sunshine
    }
    if (driver_ != cert) {
      return false;
    }
    last_input_ = now_();
    return true;
  }

  result_e baton_t::request(const std::string &cert) {
    std::string name;
    {
      std::lock_guard lock(mutex_);
      expire_idle_locked();
      auto *seat = find_locked(cert);
      if (!seat) {
        return result_e::no_stream;
      }
      if (driver_ == cert) {
        return result_e::ok;
      }
      if (driver_.empty()) {
        host_hold_ = false;
        set_driver_locked(cert);
        return result_e::ok;
      }
      seat->asked = true;
      name = seat->name;
    }
    if (on_request) {
      on_request(name);
    }
    return result_e::ok;
  }

  result_e baton_t::give(const std::string &cert, const std::string &to_id) {
    std::lock_guard lock(mutex_);
    expire_idle_locked();
    if (driver_ != cert) {
      return result_e::not_driver;
    }
    auto *to = find_by_id_locked(to_id);
    if (!to) {
      return result_e::no_such_seat;
    }
    set_driver_locked(to->cert);
    return result_e::ok;
  }

  result_e baton_t::takeback(const std::string &cert) {
    std::lock_guard lock(mutex_);
    if (!find_locked(cert)) {
      return result_e::no_stream;
    }
    if (owner_cert_locked() != cert) {
      return result_e::not_owner;
    }
    host_hold_ = false;
    set_driver_locked(cert);
    return result_e::ok;
  }

  void baton_t::host_takeback() {
    std::lock_guard lock(mutex_);
    host_hold_ = true;
    set_driver_locked({});
  }

  void baton_t::host_give(const std::string &to_id) {
    std::lock_guard lock(mutex_);
    if (auto *to = find_by_id_locked(to_id)) {
      host_hold_ = false;
      set_driver_locked(to->cert);
    }
  }

  void baton_t::host_release() {
    std::lock_guard lock(mutex_);
    host_hold_ = false;
    set_driver_locked({});
  }

  view_t baton_t::view(const std::string &cert) {
    std::lock_guard lock(mutex_);
    expire_idle_locked();
    view_t v;
    v.driving = !driver_.empty() && driver_ == cert;
    v.owner = owner_cert_locked() == cert;
    if (auto *d = find_locked(driver_)) {
      v.driver_name = d->name;
    }
    for (const auto &s : seats_) {
      if (s.cert != cert) {
        v.seats.push_back(s);
      }
    }
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

  bool baton_t::remote_driving() {
    std::lock_guard lock(mutex_);
    expire_idle_locked();
    return !driver_.empty();
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
