// SPDX-License-Identifier: GPL-3.0-or-later
// A 3270 control unit in CUT mode for one terminal: the controller end of
// a coax line, with a TN3270 session behind it, as a 3174 drives a CUT
// display [documented: 3174 behaviour; the coax command set].
//
//   TN3270 host <-> tn3270::Client (3270 data stream) <-> CutController
//                                     <-> CoaxHost line <-> terminal
//
// The controller owns the 3270 presentation space (the session's engine)
// and keeps the terminal's buffer equal to it: it identifies the terminal
// (terminal ID: screen model; feature IDs: the extended attribute buffer),
// writes only what changed (Load Address Counter + Write Data, or EAB
// Write Alternate for character and extended attribute together),
// translates the host's EBCDIC into the terminal's display code, draws the
// status line (addresses 0-79) and leaves the address counter at the
// cursor, which a CUT terminal shows there. It polls for keystrokes, which
// arrive as the keyboard's scan codes, and turns them into 3270 operator
// actions with the 5250 workstation controller's keyboard map (same
// keyboard, same codes).
#pragma once

#include "host/twinax/wsc/keyboard_map.hpp"
#include "machine/coax_port.hpp"
#include "tn3270/client.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace em34xx {

class CutController : public CoaxHost {
public:
    // Takes the session; null runs the controller without a host (the
    // terminal is identified and shown a blank screen).
    explicit CutController(std::unique_ptr<tn3270::Client> session);
    ~CutController() override;

    const char* kind() const override { return "tn3270"; }
    bool bridges_sessions() const override { return session_ != nullptr; }
    void advance(uint64_t emu_ns) override;
    void terminal_reset() override;
    std::string describe() const override;
    std::vector<LineSessionStatus> session_status() const override;

    void from_terminal(const CoaxMessage& msg) override;
    bool to_terminal(CoaxMessage& out) override;
    void no_response() override;

    // The terminal as identified (tests, monitor).
    bool terminal_ready() const { return phase_ == Phase::Running; }
    int rows() const { return rows_; }
    bool has_eab() const { return eab_; }
    bool has_eab_colour() const { return eab_colour_; }

    // One key byte from a poll response -> an operator action; false when
    // the byte only changes state (a break prefix, a shift key, a break).
    // The 3483 sends its keyboard's scan-code set 3 bytes unchanged: a
    // make code, or F0 and the make code for a break [observed].
    bool decode_key(uint8_t code, wsc::OperatorAction& out);
    const wsc::KeyboardMap& keyboard() const { return keys_; }

private:
    enum class Phase { Probe, Resetting, Identify, Running };
    enum class Sent { None, Poll, PollAck, Reset, TerminalId, FeatureId, Other };

    void queue(CoaxMessage msg, Sent kind = Sent::Other) { out_.push_back({std::move(msg), kind}); }
    void start_identify();
    void restart();
    void keystroke(uint8_t code);
    void paint();
    void paint_status();
    uint8_t display_byte(int addr) const;
    uint8_t eab_byte(int addr) const;

    std::unique_ptr<tn3270::Client> session_;
    wsc::KeyboardMap keys_{0x82};
    bool break_next_ = false;       // F0 seen: the next code is a break
    Phase phase_ = Phase::Probe;
    struct Pending {
        CoaxMessage msg;
        Sent kind;
    };
    std::deque<Pending> out_;
    Sent in_flight_ = Sent::None;
    bool waiting_ = false;          // a transmission is out, its outcome not yet reported
    uint64_t now_ = 0;
    bool probe_seen_ = false;      // the terminal answered a probe poll
    uint64_t probe_since_ = 0;     // since then (emulated ns)
    uint64_t next_poll_ = 0;
    unsigned silent_ = 0;           // transmissions in a row without an answer
    int feature_addr_ = 2;          // READ FEATURE ID probe in progress
    int rows_ = 24, cols_ = 80;
    bool eab_ = false, eab_colour_ = false;
    // What the terminal's buffer holds (address 0 = status line).
    std::vector<uint8_t> shown_, shown_eab_;
    uint64_t painted_gen_ = ~0ull;
    int shown_cursor_ = -1;
    std::string status_;
};

} // namespace em34xx
