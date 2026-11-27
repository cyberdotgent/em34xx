// SPDX-License-Identifier: GPL-3.0-or-later
// The life of one host session behind a terminal's line, whatever the line
// and the protocol: a controller (the Twinax workstation controller with
// its TN5250 sessions, the coax CUT controller with its TN3270 session)
// owns one SessionCycle per session and gives it the session through Hooks.
//
// The rules (docs/twinax-bridge.md, "Session and terminal lifecycle"):
//   - The session is the cable. The terminal is polled only while the
//     session is up (tick() returns true); otherwise it sees no controller.
//   - A session belongs to one run of the terminal. Once the terminal has
//     come on line on a session, that session ends when the terminal leaves
//     the line for any reason: reset, power-off, Offline Setup, a firmware
//     restart. The terminal then meets a new session (a new sign-on), as it
//     would on a real host. Nothing of the old one is carried over.
//   - A session that ends at the host's side (closed, failed, network gone)
//     is opened again after a wait that doubles from reconnect_min_ms to
//     reconnect_max_ms.
//   - stop(): the session is closed and stays closed until the terminal is
//     reset or powered up (a refused certificate must not be asked again
//     every few seconds).
// Everything runs on the emulation thread; tick() is called while the
// machine runs, so a powered-off terminal has no session.
#pragma once

#include <chrono>
#include <functional>
#include <string>

namespace em34xx {

enum class SessionPhase {
    Idle,         // no session, none started
    Connecting,   // being set up
    Ready,        // up: the terminal may be polled
    Down,         // ended at the host's side or failed
};

struct SessionCycleOptions {
    bool reconnect = true;              // open again after the session ends at the host's side
    unsigned reconnect_min_ms = 2000;   // wall clock
    unsigned reconnect_max_ms = 60000;
};

class SessionCycle {
public:
    struct Hooks {
        std::function<SessionPhase()> phase;
        // Start a session. Called in phase Idle or Down; a used session
        // object is replaced by the callee.
        std::function<void()> open;
        // End the session now; phase() is Idle afterwards.
        std::function<void()> close;
        // What ended the last session, for the log ("" when unknown).
        std::function<std::string()> last_error;
        // Who this is in the log: "station 0".
        std::string label;
    };

    SessionCycle(Hooks hooks, SessionCycleOptions options);

    // The terminal came on line on the current session: it is now in use.
    void terminal_on_line();
    // The terminal left the line (the controller noticed: no response, a
    // power-on status). May be called from inside the controller's own
    // line handling: the session ends at the next tick().
    void terminal_off_line();
    // The terminal was reset or switched off (the machine said so). Ends a
    // session that was in use at once and lifts stop().
    void terminal_reset();
    // Close the session and keep it closed until terminal_reset().
    void stop();
    bool stopped() const { return stopped_; }

    // Drive the session; true while the terminal may be polled (the cable
    // is plugged).
    bool tick();

    unsigned sessions_started() const { return starts_; }

private:
    void open();
    void end_used_session(const char* why);

    Hooks hooks_;
    SessionCycleOptions opt_;
    bool in_use_ = false;        // the terminal has been on line on this session
    bool end_pending_ = false;   // terminal_off_line(): end at the next tick
    bool stopped_ = false;
    unsigned starts_ = 0;
    unsigned backoff_ms_ = 0;
    std::chrono::steady_clock::time_point retry_at_{};
    SessionPhase last_ = SessionPhase::Idle;
};

} // namespace em34xx
