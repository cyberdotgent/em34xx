// The boundary between the emulated terminal's Twinax hardware and whatever
// plays the controller (the primary station) on the other end of the cable.
//
//   firmware <-> [model-specific link device] <-> TwinaxHost <-> network/script
//
// This file fixes the protocol unit both sides exchange (the 16-bit 5250
// workstation frame) and the controller contract. It belongs to the
// emulator core, which owns the line; the controllers themselves live in
// src/host (tn34xx_host, tn34xx_bridge).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tn34xx {

// One 5250 workstation frame. On the wire (after the bit-sync and code
// violation preamble) it is 16 bit cells:
//
//   bit 15     sync, always 1
//   bits 14..7 data/command byte, least significant bit first
//   bits 6..4  station address, least significant bit first (7 = end of
//              message); address bit 0 is frame bit 6
//   bit 3      even parity over the active bits
//   bits 2..0  fill, 000
struct TwinaxFrame {
    uint8_t data = 0;
    uint8_t address = 0;   // 0..7

    bool end_of_message() const { return address == 7; }

    uint16_t encode() const;
    // Returns false if the sync bit is missing. parity_ok reports the parity
    // check separately so callers can decide how to treat parity errors.
    static bool decode(uint16_t word, TwinaxFrame& out, bool& parity_ok);
};

using TwinaxMessage = std::vector<TwinaxFrame>;

std::string format_twinax_message(const TwinaxMessage& msg);

// The station a controller -> terminal message is for: the first frame
// carries the station address and selects the station; later frames belong
// to it whatever their address field (7 = end of message). Returns 7 for an
// empty message.
inline uint8_t twinax_message_station(const TwinaxMessage& msg) {
    return msg.empty() ? uint8_t(7) : msg.front().address;
}

// The controller side of the line.
//
// Contract with the terminal's link device (which owns line timing):
//   - The line is half duplex. The link device calls to_station() only while
//     the line is idle. After it transmits a message, it reports exactly one
//     outcome before it calls to_station() again: from_station() with the
//     addressed station's response, or no_response() when no station
//     answered within the turnaround window. Messages that need no response
//     (an Activate Write, or a queue load) are acknowledged with
//     no_response() as well, after their transmit time.
//   - A response therefore always belongs to the station that
//     twinax_message_station() names for the last message sent; its frames
//     carry address 7 on the last frame and are not otherwise addressed.
//   - advance() feeds the emulated clock. The link device calls it with the
//     machine's emulated time before each to_station() and whenever its own
//     time moves on while the line is idle, so controller timers (poll
//     cadence, busy timeouts, the terminal's 200 ms System Available window)
//     follow emulated time, not wall time.
// Several station addresses (dual-session terminals, a printer session) share
// one host: the host addresses each message to one of them.
class TwinaxHost {
public:
    virtual ~TwinaxHost() = default;
    virtual const char* kind() const = 0;

    // Terminal -> controller (a response or status message) for the last
    // message sent.
    virtual void from_station(const TwinaxMessage& msg) = 0;
    // Controller -> terminal. Non-blocking; returns false when the host has
    // nothing to send at the current emulated time.
    virtual bool to_station(TwinaxMessage& out) = 0;
    // The last message sent got no response (see the contract above).
    virtual void no_response() {}
    // Emulated time in nanoseconds since machine power-on; monotonic.
    virtual void advance(uint64_t emu_ns) { (void)emu_ns; }

    virtual std::string describe() const = 0;
    // Close the host session behind one station address and keep it closed
    // (monitor `disconnect N`). False if this host has no such session.
    virtual bool disconnect_station(unsigned address) { (void)address; return false; }
};

// No controller attached: the line is silent.
class NullTwinaxHost : public TwinaxHost {
public:
    const char* kind() const override { return "none"; }
    void from_station(const TwinaxMessage&) override {}
    bool to_station(TwinaxMessage&) override { return false; }
    std::string describe() const override { return "twinax: no controller attached\n"; }
};

} // namespace tn34xx
