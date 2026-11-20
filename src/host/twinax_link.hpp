// The boundary between the emulated terminal's Twinax hardware and whatever
// plays the controller (the primary station) on the other end of the cable.
//
//   firmware <-> [model-specific link device] <-> TwinaxHost <-> network/script
//
// The link device is the part we still have to discover from the firmware:
// its registers, status bits and interrupts. This file only fixes the
// protocol unit both sides exchange: the 16-bit 5250 workstation frame.
#pragma once

#include "host/host_config.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tn34xx {

// One 5250 workstation frame. On the wire (after the bit-sync and code
// violation preamble) it is 16 bit cells:
//
//   bit 15     sync, always 1
//   bits 14..7 data/command byte, least significant bit first
//   bits 6..4  station address (7 = end of message)
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

// The controller side of the line.
class TwinaxHost {
public:
    virtual ~TwinaxHost() = default;
    virtual const char* kind() const = 0;

    // Terminal -> controller (a response or status message).
    virtual void from_station(const TwinaxMessage& msg) = 0;
    // Controller -> terminal. Non-blocking; returns false when idle.
    virtual bool to_station(TwinaxMessage& out) = 0;

    virtual std::string describe() const = 0;
};

// No controller attached: the line is silent.
class NullTwinaxHost : public TwinaxHost {
public:
    const char* kind() const override { return "none"; }
    void from_station(const TwinaxMessage&) override {}
    bool to_station(TwinaxMessage&) override { return false; }
    std::string describe() const override { return "twinax: no controller attached\n"; }
};

// Frames are queued by the monitor and responses are recorded for
// inspection. This is the bring-up path for the link device.
class ScriptTwinaxHost : public TwinaxHost {
public:
    const char* kind() const override { return "script"; }
    void from_station(const TwinaxMessage& msg) override;
    bool to_station(TwinaxMessage& out) override;
    std::string describe() const override;

    void queue(TwinaxMessage msg);
    std::vector<TwinaxMessage> take_received();

private:
    mutable std::mutex mutex_;
    std::deque<TwinaxMessage> outgoing_;
    std::vector<TwinaxMessage> received_;
    uint64_t sent_count_ = 0;
};

// Placeholder for the TN5250 bridge. It validates and reports its
// configuration but does not connect yet.
class Tn5250TwinaxHost : public TwinaxHost {
public:
    explicit Tn5250TwinaxHost(Tn5250Config cfg) : cfg_(std::move(cfg)) {}
    const char* kind() const override { return "tn5250"; }
    void from_station(const TwinaxMessage& msg) override;
    bool to_station(TwinaxMessage&) override { return false; }
    std::string describe() const override;

private:
    Tn5250Config cfg_;
};

std::unique_ptr<TwinaxHost> make_twinax_host(const TwinaxConfig& twinax, const Tn5250Config& tn5250,
                                             std::string& error);

} // namespace tn34xx
