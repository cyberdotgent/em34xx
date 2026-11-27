// SPDX-License-Identifier: GPL-3.0-or-later
// The line a terminal hangs on, and the controller at its other end.
//
// A model states its line (ModelInfo::line). Each kind of line has its own
// host interface, because what travels on it differs: TwinaxHost
// (machine/twinax_port.hpp) exchanges Twinax frames with the terminal's
// link engine. A coax terminal has a CoaxHost (machine/coax_port.hpp), the
// CUT controller in src/host/coax, which wraps a TN3270 session the way the
// workstation controller in src/host/twinax wraps TN5250. What every line host has in common is
// here, so the machine, the application and the front ends hold a LineHost
// and do not care which line it is.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace em34xx {

enum class LineKind {
    Twinax,   // IBM 5250 family: polled stations on a twinaxial cable
    Coax,     // IBM 3270 family: one terminal per coaxial port (no model yet)
};

inline const char* line_kind_name(LineKind k) { return k == LineKind::Twinax ? "twinax" : "coax"; }

// One host session behind a station address, for a status bar.
struct LineSessionStatus {
    unsigned address = 0;
    bool printer = false;
    std::string state;        // "connecting", "ready", "failed", "disconnected"...
    bool connected = false;   // session up (the cable is plugged)
    bool held = false;        // closed until the terminal is reset (a refused certificate)
    std::string peer;         // "host:port"
    bool tls = false;
    std::string error;        // last error, if any
};

class LineHost {
public:
    virtual ~LineHost() = default;
    // The line this host drives; a machine only takes a host for its own.
    virtual LineKind line() const = 0;
    // What is at the other end: "none", "script", "wsc", "tn5250", ...
    virtual const char* kind() const = 0;
    // Emulated time in nanoseconds since machine power-on; monotonic.
    virtual void advance(uint64_t emu_ns) { (void)emu_ns; }

    // Whether host sessions hang off this controller (a bridge to TN5250 or
    // TN3270), as opposed to a test controller or none.
    virtual bool bridges_sessions() const { return false; }
    // The terminal was reset or switched off (Machine::reset(), power-off).
    // A host session belongs to one run of the terminal: sessions it was
    // using end here, and it meets new ones when it comes back, as on a
    // real host. Called with the machine lock held.
    virtual void terminal_reset() {}

    virtual std::string describe() const = 0;
    // Close the host session behind one station address and keep it closed
    // until the terminal is reset (a refused certificate, monitor
    // `disconnect N`). False if this host has no such session.
    virtual bool disconnect_station(unsigned address) { (void)address; return false; }
    // The host sessions behind the stations, if this host has any.
    virtual std::vector<LineSessionStatus> session_status() const { return {}; }
};

// No controller attached: the line is silent. For a line without a host
// interface of its own yet.
class NullLineHost : public LineHost {
public:
    explicit NullLineHost(LineKind line) : line_(line) {}
    LineKind line() const override { return line_; }
    const char* kind() const override { return "none"; }
    std::string describe() const override { return std::string(line_kind_name(line_)) + ": no controller attached\n"; }

private:
    LineKind line_;
};

} // namespace em34xx
