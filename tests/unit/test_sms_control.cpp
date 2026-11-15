/**
 * @file tests/unit/test_sms_control.cpp
 * @brief Simpleminded Screens: the control baton.
 */
#include "../tests_common.h"

#include <chrono>

#include <src/sms/control.h>

using namespace sms::control;
using namespace std::chrono_literals;

namespace {
  struct fake_clock_t {
    steady_clock_t::time_point t {};
  };

  struct fixture_t {
    fake_clock_t clock;
    baton_t baton {[this] { return clock.t; }};
    std::vector<std::string> changes;  // old driver certs
    std::vector<std::string> requests;  // names

    fixture_t() {
      baton.on_driver_change = [this](const std::string &old) { changes.push_back(old); };
      baton.on_request = [this](const std::string &name) { requests.push_back(name); };
    }

    void tick(steady_clock_t::duration d) {
      clock.t += d;
    }
  };
}  // namespace

TEST(SmsControl, SeatIdIsTwelveHexCharsAndStable) {
  const auto id = seat_id("CERT-A");
  EXPECT_EQ(id.size(), 12u);
  EXPECT_EQ(id, seat_id("CERT-A"));
  EXPECT_NE(id, seat_id("CERT-B"));
  EXPECT_EQ(id.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(SmsControl, FirstSeatDrivesOthersWatch) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.tick(1s);
  f.baton.join("B", "Lee");
  EXPECT_TRUE(f.baton.may_input("A"));
  EXPECT_FALSE(f.baton.may_input("B"));
  EXPECT_TRUE(f.baton.view("A").driving);
  EXPECT_EQ(f.baton.view("B").driver_name, "Tim");
}

TEST(SmsControl, RequestWhileDrivenMarksAskedAndTellsHost) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  EXPECT_EQ(f.baton.request("B"), result_e::ok);
  EXPECT_FALSE(f.baton.may_input("B"));
  ASSERT_EQ(f.requests.size(), 1u);
  EXPECT_EQ(f.requests[0], "Lee");
  const auto seats = f.baton.view("A").seats;
  ASSERT_EQ(seats.size(), 1u);
  EXPECT_TRUE(seats[0].asked);
}

TEST(SmsControl, RequestWhenFreeTakesControl) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  f.baton.host_takeback();
  EXPECT_EQ(f.baton.request("B"), result_e::ok);
  EXPECT_TRUE(f.baton.may_input("B"));
}

TEST(SmsControl, OnlyTheDriverGives) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  EXPECT_EQ(f.baton.give("B", seat_id("A")), result_e::not_driver);
  EXPECT_EQ(f.baton.give("A", seat_id("nobody")), result_e::no_such_seat);
  EXPECT_EQ(f.baton.give("A", seat_id("B")), result_e::ok);
  EXPECT_TRUE(f.baton.may_input("B"));
  EXPECT_FALSE(f.baton.may_input("A"));
  ASSERT_EQ(f.changes.size(), 1u);
  EXPECT_EQ(f.changes[0], "A");  // A's held keys must be released
}

TEST(SmsControl, OwnerIsTickedSeatElseFirstToJoin) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.tick(1s);
  f.baton.join("B", "Lee");
  EXPECT_TRUE(f.baton.view("A").owner);
  EXPECT_FALSE(f.baton.view("B").owner);
  f.baton.set_owner_cert("B");
  EXPECT_TRUE(f.baton.view("B").owner);
  EXPECT_FALSE(f.baton.view("A").owner);
  f.baton.set_owner_cert("C");  // ticked owner not connected
  EXPECT_TRUE(f.baton.view("A").owner);
}

TEST(SmsControl, OnlyTheOwnerTakesBack) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.tick(1s);
  f.baton.join("B", "Lee");
  ASSERT_EQ(f.baton.give("A", seat_id("B")), result_e::ok);
  EXPECT_EQ(f.baton.takeback("B"), result_e::not_owner);
  EXPECT_EQ(f.baton.takeback("A"), result_e::ok);
  EXPECT_TRUE(f.baton.may_input("A"));
}

TEST(SmsControl, HostTrayTakebackFreesAndHoldsEvenForOneSeat) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.baton.host_takeback();
  EXPECT_FALSE(f.baton.may_input("A"));
  EXPECT_FALSE(f.baton.remote_driving());
  EXPECT_EQ(f.baton.request("A"), result_e::ok);
  EXPECT_TRUE(f.baton.may_input("A"));
}

TEST(SmsControl, HostGivesToAskedSeat) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  f.baton.request("B");
  ASSERT_EQ(f.baton.asked_seats().size(), 1u);
  f.baton.host_give(seat_id("B"));
  EXPECT_TRUE(f.baton.may_input("B"));
  EXPECT_TRUE(f.baton.asked_seats().empty());
}

TEST(SmsControl, IdleDriverLosesControl) {
  fixture_t f;
  f.baton.set_idle_limit(5min);
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  f.tick(4min);
  EXPECT_TRUE(f.baton.may_input("A"));  // input resets the idle clock
  f.tick(4min);
  EXPECT_EQ(f.baton.view("B").driver_name, "Tim");
  f.tick(2min);
  EXPECT_EQ(f.baton.view("B").driver_name, "");  // free after 5 min without input
  EXPECT_EQ(f.baton.request("B"), result_e::ok);
  EXPECT_TRUE(f.baton.may_input("B"));
}

TEST(SmsControl, DriverLeavingFreesControlAndAloneSeatDrives) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  f.baton.leave("A");
  EXPECT_TRUE(f.baton.may_input("B"));  // alone again: drives as today
}

TEST(SmsControl, NoSeatsMeansNoStream) {
  fixture_t f;
  EXPECT_EQ(f.baton.request("A"), result_e::no_stream);
}

TEST(SmsControl, ReplyCarriesStateAndRefusals) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  f.baton.request("B");
  const auto ok = control_reply(result_e::ok, f.baton.view("A"));
  EXPECT_EQ(ok.get<int>("root.<xmlattr>.status_code"), 200);
  EXPECT_EQ(ok.get<std::string>("root.me"), "driver");
  EXPECT_EQ(ok.get<int>("root.owner"), 1);
  EXPECT_EQ(ok.get<std::string>("root.driver"), "Tim");
  EXPECT_EQ(ok.get<std::string>("root.seat.name"), "Lee");
  EXPECT_EQ(ok.get<std::string>("root.seat.id"), seat_id("B"));
  EXPECT_EQ(ok.get<int>("root.seat.asked"), 1);
  auto refused = [&](result_e r, int code, const std::string &msg) {
    const auto t = control_reply(r, f.baton.view("A"));
    EXPECT_EQ(t.get<int>("root.<xmlattr>.status_code"), code);
    EXPECT_EQ(t.get<std::string>("root.<xmlattr>.status_message"), msg);
  };
  refused(result_e::not_driver, 403, "Only the driver can give control");
  refused(result_e::not_owner, 403, "Only the owner can take control back");
  refused(result_e::no_such_seat, 404, "That viewer is no longer connected");
  refused(result_e::no_stream, 409, "No stream is running");
}

TEST(SmsControl, DriverChangeCallbackRunsOutsideTheLock) {
  fixture_t f;
  f.baton.on_driver_change = [&](const std::string &old) {
    f.changes.push_back(old);
    f.baton.view("A");  // would self-deadlock if the lock were still held
  };
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  EXPECT_EQ(f.baton.give("A", seat_id("B")), result_e::ok);
  f.baton.host_takeback();
  f.baton.host_release();
  EXPECT_EQ(f.changes.size(), 2u);
}

TEST(SmsControl, UnknownOrEmptyCertNeverDrives) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  f.baton.host_release();  // control is free
  EXPECT_FALSE(f.baton.may_input(""));
  EXPECT_FALSE(f.baton.may_input("nobody"));
  EXPECT_EQ(f.baton.give("", seat_id("B")), result_e::not_driver);
  EXPECT_EQ(f.baton.give("nobody", seat_id("B")), result_e::not_driver);
  EXPECT_FALSE(f.baton.remote_driving());
}

TEST(SmsControl, HostTakebackWithNoSeatsDoesNotHoldTheNextSeat) {
  fixture_t f;
  f.baton.host_takeback();
  f.baton.join("A", "Tim");
  EXPECT_TRUE(f.baton.may_input("A"));
}

TEST(SmsControl, RepeatRequestTellsHostOnce) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  f.baton.request("B");
  f.baton.request("B");
  EXPECT_EQ(f.requests.size(), 1u);
}

TEST(SmsControl, SecondSeatJoiningResetsIdleClock) {
  fixture_t f;
  f.baton.set_idle_limit(5min);
  f.baton.join("A", "Tim");
  f.tick(10min);
  f.baton.join("B", "Lee");
  EXPECT_TRUE(f.baton.may_input("A"));
}

TEST(SmsControl, LeavingClearsAsked) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  f.baton.request("B");
  f.baton.leave("B");
  EXPECT_TRUE(f.baton.asked_seats().empty());
}

TEST(SmsControl, DriverChangeFiresOnHostTakebackAndIdleExpiryNotOnDriverLeaving) {
  fixture_t f;
  f.baton.set_idle_limit(5min);
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  f.baton.host_takeback();
  ASSERT_EQ(f.changes.size(), 1u);
  EXPECT_EQ(f.changes[0], "A");
  f.baton.request("B");  // free: B drives
  f.tick(6min);
  EXPECT_FALSE(f.baton.remote_driving());  // idle expiry
  ASSERT_EQ(f.changes.size(), 2u);
  EXPECT_EQ(f.changes[1], "B");
  f.baton.request("A");
  f.baton.leave("A");  // driver leaves: nothing to release
  EXPECT_EQ(f.changes.size(), 2u);
}

TEST(SmsControl, HostReleaseFreesControl) {
  fixture_t f;
  f.baton.join("A", "Tim");
  f.baton.join("B", "Lee");
  f.baton.host_release();
  EXPECT_FALSE(f.baton.remote_driving());
  EXPECT_EQ(f.changes.size(), 1u);
  EXPECT_EQ(f.baton.request("B"), result_e::ok);
  EXPECT_TRUE(f.baton.may_input("B"));
}
