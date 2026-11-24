// SPDX-License-Identifier: GPL-3.0-or-later
// The life of a host session behind a terminal's line (host/session_cycle.hpp),
// with a scripted session instead of a TN5250 client: the rules are not
// specific to a line or a protocol.
#include "host/session_cycle.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <thread>

using namespace em34xx;

namespace {
struct Session {
    SessionPhase phase = SessionPhase::Idle;
    int opened = 0, closed = 0;
    SessionCycle::Hooks hooks() {
        return {[this] { return phase; },
                [this] {
                    ++opened;
                    phase = SessionPhase::Connecting;
                },
                [this] {
                    ++closed;
                    phase = SessionPhase::Idle;
                },
                [] { return std::string("test"); }, "station 9"};
    }
};
} // namespace

TEST_CASE("session cycle: the session is the cable") {
    Session s;
    SessionCycle c(s.hooks(), SessionCycleOptions{});
    CHECK_FALSE(c.tick());   // opened, not up yet
    CHECK(s.opened == 1);
    CHECK_FALSE(c.tick());
    CHECK(s.opened == 1);
    s.phase = SessionPhase::Ready;
    CHECK(c.tick());
    CHECK(c.sessions_started() == 1);
}

TEST_CASE("session cycle: a session ends with the run of the terminal that used it") {
    Session s;
    SessionCycle c(s.hooks(), SessionCycleOptions{});
    c.tick();
    s.phase = SessionPhase::Ready;
    REQUIRE(c.tick());

    // Not on line yet: the session is not in use, a reset keeps it.
    c.terminal_reset();
    c.terminal_off_line();
    CHECK(c.tick());
    CHECK(s.closed == 0);

    // On line, then gone from the line: the session is closed at the next
    // tick and a new one opened; the cable is out until that one is up.
    c.terminal_on_line();
    c.terminal_off_line();
    CHECK(s.closed == 0);   // not from inside the caller's line handling
    CHECK_FALSE(c.tick());
    CHECK(s.closed == 1);
    CHECK(s.opened == 2);
    s.phase = SessionPhase::Ready;
    CHECK(c.tick());

    // Told directly (reset, power-off): closed at once.
    c.terminal_on_line();
    c.terminal_reset();
    CHECK(s.closed == 2);
    CHECK(s.phase == SessionPhase::Idle);
    CHECK(s.opened == 2);    // a new one only when the machine runs again
    CHECK_FALSE(c.tick());
    CHECK(s.opened == 3);
}

TEST_CASE("session cycle: a session lost at the host's side is reopened after a wait") {
    Session s;
    SessionCycleOptions o;
    o.reconnect_min_ms = 50;
    o.reconnect_max_ms = 50;
    SessionCycle c(s.hooks(), o);
    c.tick();
    s.phase = SessionPhase::Ready;
    c.tick();
    c.terminal_on_line();
    // The host hangs up; the controller then pulls the cable and reports the
    // terminal gone, which must not restart the session at once.
    s.phase = SessionPhase::Down;
    c.terminal_off_line();
    CHECK_FALSE(c.tick());
    CHECK_FALSE(c.tick());
    CHECK(s.opened == 1);
    CHECK(s.closed == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK_FALSE(c.tick());
    CHECK(s.opened == 2);

    // Without reconnect it stays down.
    Session t;
    o.reconnect = false;
    SessionCycle d(t.hooks(), o);
    d.tick();
    t.phase = SessionPhase::Down;
    d.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    d.tick();
    CHECK(t.opened == 1);
}

TEST_CASE("session cycle: stopped until the terminal is reset") {
    Session s;
    SessionCycle c(s.hooks(), SessionCycleOptions{});
    c.tick();
    s.phase = SessionPhase::Ready;
    REQUIRE(c.tick());
    c.stop();
    CHECK(c.stopped());
    CHECK(s.closed == 1);
    CHECK_FALSE(c.tick());
    CHECK(s.opened == 1);
    c.terminal_reset();
    CHECK_FALSE(c.stopped());
    CHECK_FALSE(c.tick());
    CHECK(s.opened == 2);
}
