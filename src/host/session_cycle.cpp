// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/session_cycle.hpp"

#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>

namespace em34xx {

SessionCycle::SessionCycle(Hooks hooks, SessionCycleOptions options) : hooks_(std::move(hooks)), opt_(options) {}

void SessionCycle::open() {
    ++starts_;
    in_use_ = false;
    end_pending_ = false;
    last_ = SessionPhase::Connecting;
    hooks_.open();
}

void SessionCycle::end_used_session(const char* why) {
    LOG_INFO("host", "%s: %s; its session is closed", hooks_.label.c_str(), why);
    hooks_.close();
    in_use_ = false;
    end_pending_ = false;
    backoff_ms_ = 0;
    last_ = SessionPhase::Idle;
}

void SessionCycle::terminal_on_line() {
    in_use_ = true;
    end_pending_ = false;
}

void SessionCycle::terminal_off_line() {
    // Only a session that is still up is ended here: when the session went
    // first (the cable was pulled because of it), the reconnect wait applies.
    if (in_use_ && hooks_.phase() == SessionPhase::Ready) end_pending_ = true;
}

void SessionCycle::terminal_reset() {
    stopped_ = false;
    if (in_use_) end_used_session("the terminal was reset or switched off");
}

void SessionCycle::stop() {
    if (stopped_) return;
    stopped_ = true;
    LOG_INFO("host", "%s: session closed until the terminal is reset", hooks_.label.c_str());
    hooks_.close();
    in_use_ = false;
    end_pending_ = false;
    last_ = SessionPhase::Idle;
}

bool SessionCycle::tick() {
    if (stopped_) return false;
    if (end_pending_) end_used_session("the terminal left the line");
    SessionPhase ph = hooks_.phase();
    if (ph == SessionPhase::Idle) {
        open();
        ph = hooks_.phase();
    }
    if (ph != last_) {
        if (ph == SessionPhase::Ready) {
            LOG_INFO("host", "%s: session connected", hooks_.label.c_str());
            backoff_ms_ = 0;
        } else if (ph == SessionPhase::Down) {
            const std::string err = hooks_.last_error ? hooks_.last_error() : std::string();
            backoff_ms_ = backoff_ms_ ? std::min(backoff_ms_ * 2, opt_.reconnect_max_ms) : opt_.reconnect_min_ms;
            retry_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoff_ms_);
            LOG_WARN("host", "%s: session ended%s%s%s", hooks_.label.c_str(), err.empty() ? "" : ": ", err.c_str(),
                     opt_.reconnect ? format("; reconnecting in %u ms", backoff_ms_).c_str() : "");
        }
        last_ = ph;
    }
    if (ph == SessionPhase::Down && opt_.reconnect && std::chrono::steady_clock::now() >= retry_at_) {
        open();
        ph = hooks_.phase();
    }
    return ph == SessionPhase::Ready;
}

} // namespace em34xx
