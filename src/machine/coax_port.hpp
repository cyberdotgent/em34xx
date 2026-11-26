// SPDX-License-Identifier: GPL-3.0-or-later
// The boundary between the emulated terminal's coax hardware and whatever
// plays the controller (a 3174/3274 in CUT mode) on the other end of the
// cable.
//
//   firmware <-> [model-specific coax adapter] <-> CoaxHost <-> network
//
// This file fixes the unit both sides exchange (the 10-bit 3270 coax word)
// and the controller contract, as twinax_port.hpp does for Twinax. The
// controllers live in src/host/coax.
//
// A 3270 coax word [documented: 3270 coax protocol descriptions; bit 9 is
// sent first after the sync bit]:
//   command word  bits 9..2 = command, bit 1 = 0, bit 0 = 1
//   data word     bits 9..2 = data byte, bit 1 = parity, bit 0 = 0
//   TT/AR         all zero: the turnaround (auto response) a terminal sends
//                 when it has nothing else to answer with
#pragma once

#include "machine/line_port.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace em34xx {

struct CoaxWord {
    uint16_t bits = 0;   // 10 bits as above

    static CoaxWord command(uint8_t cmd) { return CoaxWord{uint16_t(cmd << 2 | 1)}; }
    // A data word with odd parity over the data byte and the parity bit.
    static CoaxWord data(uint8_t byte);
    static CoaxWord turnaround() { return CoaxWord{0}; }

    bool is_command() const { return (bits & 1) != 0; }
    bool is_turnaround() const { return bits == 0; }
    uint8_t byte() const { return uint8_t(bits >> 2); }
};

inline CoaxWord CoaxWord::data(uint8_t byte) {
    bool odd = false;
    for (uint8_t b = byte; b; b &= uint8_t(b - 1)) odd = !odd;
    return CoaxWord{uint16_t(byte << 2 | (odd ? 0 : 2))};
}

using CoaxMessage = std::vector<CoaxWord>;

std::string format_coax_message(const CoaxMessage& msg);

// The controller side of the line.
//
// Contract with the terminal's coax adapter (which owns line timing):
//   - The line is half duplex and the controller always speaks first. The
//     adapter calls to_terminal() only while the line is idle. After it has
//     taken a transmission (a command word and the data words that follow
//     it), it reports exactly one outcome before it calls to_terminal()
//     again: from_terminal() with the terminal's response (at least one
//     word; TT/AR when it has nothing to say), or no_response() when the
//     terminal does not answer (off line, powered down, in reset).
//   - advance() feeds the emulated clock, as for Twinax: before each
//     to_terminal() and while the line is idle, so the controller's poll
//     cadence and timeouts follow emulated time.
class CoaxHost : public LineHost {
public:
    LineKind line() const final { return LineKind::Coax; }

    // Terminal -> controller: the response to the last transmission.
    virtual void from_terminal(const CoaxMessage& msg) = 0;
    // Controller -> terminal. Non-blocking; returns false when the
    // controller has nothing to send at the current emulated time.
    virtual bool to_terminal(CoaxMessage& out) = 0;
    // The last transmission got no response.
    virtual void no_response() {}
};

// No controller attached: the line is silent.
class NullCoaxHost : public CoaxHost {
public:
    const char* kind() const override { return "none"; }
    void from_terminal(const CoaxMessage&) override {}
    bool to_terminal(CoaxMessage&) override { return false; }
    std::string describe() const override { return "coax: no controller attached\n"; }
};

} // namespace em34xx
