// Twinax link engine of the IBM 3477 family (one engine, N station channels).
//
// The engine sits between the Twinax line and the terminal's RAM. It is a
// bus master: its command-class table, its per-channel programs and its
// per-channel descriptors all live in ordinary RAM, written by the firmware.
// For each controller message addressed to one of its station addresses it
// looks the command up in the class table and runs that channel's program
// for the class. The programs move descriptor bytes in and out of a small
// register file and start the data movers:
//
//   - Poll is answered by the engine alone, from the descriptor's status
//     (exception) and keyboard bytes plus the engine's own busy, line
//     parity, toggle and response-level bits.
//   - Activate Write and Activate Read move data between the line and the
//     buffer the descriptor names.
//   - Every other command, and its data frames, is appended to the
//     channel's command queue in RAM for the firmware to interpret.
//
// Channel events are reported as request bits (bit 0 = event, bits 1-6 =
// status) through a callback, so the owning system decides how they reach
// the CPU. See docs/bringup-3477.md for the register map and the evidence
// (observed / hypothesis) behind each behaviour.
//
// Line side: the engine is the terminal end of the TwinaxHost contract in
// machine/twinax_port.hpp. Line time is counted in CPU clocks from tick().
#pragma once

#include "bus/device.hpp"
#include "devices/ibm3477/system.hpp"
#include "machine/twinax_port.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tn34xx {

class Bus;

// Where the engine's RAM structures live.
struct TwinaxEngineLayout {
    uint32_t class_table = 0x0060;     // 32 bytes, indexed by command & 1F
    uint32_t programs = 0x0080;        // 4 programs x 32 bytes per channel
    uint32_t program_stride = 0x80;    // distance between channels' program sets
    uint32_t descriptors = 0x0180;     // 16 bytes per channel
};

class TwinaxEngine : public Device {
public:
    // Request bits a channel raises.
    static constexpr uint8_t kEvent = 0x01;          // poll response sent
    static constexpr uint8_t kQueued = 0x02;         // command(s) queued
    static constexpr uint8_t kActivateDone = 0x04;   // activate transfer ended
    static constexpr uint8_t kEndOfMessage = 0x08;   // received message ended (queue or write)
    static constexpr uint8_t kOverrun = 0x10;        // more frames than room
    static constexpr uint8_t kInvalidActivate = 0x20;
    static constexpr uint8_t kLineError = 0x40;      // frame with bad parity

    // Descriptor fields used by the engine itself (the rest is reached only
    // through the programs).
    static constexpr unsigned kDescFunction = 0x0A;  // bits 2-3: two-frame poll response

    using RaiseFn = std::function<void(unsigned channel, uint8_t bits)>;
    using PendingFn = std::function<uint8_t(unsigned channel)>;
    using HostFn = std::function<TwinaxHost*()>;

    TwinaxEngine(Bus& bus, unsigned channels, uint64_t clock_hz, TwinaxEngineLayout layout = {});

    unsigned channels() const { return unsigned(ch_.size()); }

    // Wiring. raise() sets request bits; pending() reads back the channel's
    // unacknowledged request bits (bits 0-6). host() is asked for the
    // controller each time the line goes idle, so the host can be attached
    // after the engine is built.
    void set_raise(RaiseFn f) { raise_ = std::move(f); }
    void set_pending(PendingFn f) { pending_ = std::move(f); }
    void set_host(HostFn f) { host_ = std::move(f); }

    // --- registers (the owning system decodes the addresses) -----------
    void set_station_address(unsigned ch, uint8_t addr) { ch_.at(ch).address = uint8_t(addr & 7); }
    uint8_t station_address(unsigned ch) const { return ch_.at(ch).address; }
    void set_channel_enabled(unsigned ch, bool on) { ch_.at(ch).enabled = on; }
    bool channel_enabled(unsigned ch) const { return ch_.at(ch).enabled; }
    // Run (true) or hold the engine. Held, it takes nothing from the line.
    void set_running(bool on) { running_ = on; }
    bool running() const { return running_; }
    // Channel control: write bit 7 = drop busy, bit 4 = new keyboard byte
    // posted; read bit 7 = busy.
    void write_control(unsigned ch, uint8_t v);
    uint8_t read_control(unsigned ch) const;

    // --- loopback test path ----------------------------------------------
    // While wrap is on the line is disconnected from the controller and the
    // engine's own frame generator drives it.
    void set_wrap(bool on) { wrap_ = on; }
    bool wrap() const { return wrap_; }
    // Transmit a message from the frame generator into the stations. With
    // bad_parity every frame fails the parity check.
    void inject(const TwinaxMessage& msg, bool bad_parity);
    // Data byte of the last frame seen by the loopback receiver.
    uint8_t loopback_latch() const { return latch_; }

    // --- state for tests and the monitor ---------------------------------
    bool busy(unsigned ch) const { return ch_.at(ch).busy; }
    bool line_parity_error(unsigned ch) const { return ch_.at(ch).lpe; }
    uint64_t now_cycles() const { return now_; }
    // Bus-master access to RAM (16-bit linear addresses, as the engine's
    // address registers are 16 bits wide).
    uint8_t dma_read(uint32_t a) const { return ram(a); }

    void reset() override;
    void tick(uint64_t cycles) override;
    std::string describe() const override;

    // Bit time of the line (1 Mbit/s) in CPU clocks, and the station's
    // response turnaround.
    uint64_t bit_cycles() const { return bit_cycles_; }
    uint64_t message_cycles(size_t frames) const;
    uint64_t turnaround_cycles() const { return bit_cycles_ * 45; }

private:
    struct Channel {
        uint8_t address = 0;
        bool enabled = false;
        bool busy = false;          // set by a data transfer, dropped by the firmware
        bool lpe = false;           // line parity error latched
        bool toggle = false;        // flips on every poll response
        bool level = false;         // response level
        bool new_byte2 = false;     // firmware posted a new keyboard byte
        uint64_t polls = 0, queued = 0, activates = 0;
    };

    enum class LineState { Idle, Receiving, Responding };

    // Run one received message; returns the station's response (empty for
    // none).
    TwinaxMessage receive(const TwinaxMessage& msg, bool bad_parity);
    TwinaxMessage run_program(unsigned ch, const TwinaxMessage& msg, uint8_t cls);
    TwinaxMessage poll_response(unsigned ch, uint8_t cmd);
    uint8_t ram(uint32_t a) const;
    void set_ram(uint32_t a, uint8_t v);
    uint32_t desc(unsigned ch, unsigned off) const { return layout_.descriptors + 16 * ch + off; }
    void raise(unsigned ch, uint8_t bits);
    uint8_t pending(unsigned ch) const;
    TwinaxHost* host() const { return host_ ? host_() : nullptr; }

    Bus& bus_;
    TwinaxEngineLayout layout_;
    std::vector<Channel> ch_;
    RaiseFn raise_;
    PendingFn pending_;
    HostFn host_;
    uint64_t clock_hz_;
    uint64_t bit_cycles_;

    std::array<uint8_t, 16> reg_{};   // register file shared by the programs
    bool running_ = false;
    bool wrap_ = false;
    uint8_t latch_ = 0;

    uint64_t now_ = 0;                // CPU clocks since reset
    LineState line_ = LineState::Idle;
    uint64_t line_until_ = 0;
    uint64_t next_host_check_ = 0;
    TwinaxMessage in_flight_;         // message being received
    TwinaxMessage response_;          // response being sent
    uint64_t host_messages_ = 0, host_responses_ = 0;
    bool warned_opcode_ = false;
};

// The 3477's view of the engine through the system gate array: channel
// control at 20000/20001, station addresses at 20008, channel enables and
// engine run at 20009, the loopback test registers at 20020-2002B and the
// test control bits of 20015. Offsets are relative to 20000.
class Ibm3477TwinaxRegisters : public Device, public Ibm3477TwinaxPort {
public:
    explicit Ibm3477TwinaxRegisters(TwinaxEngine& engine) : Device("twinax-regs"), eng_(engine) {}
    uint8_t twinax_read(uint32_t offset) override;
    void twinax_write(uint32_t offset, uint8_t value) override;
    void reset() override;
    std::string describe() const override;

private:
    void transmit(bool dma, bool bad_parity);

    TwinaxEngine& eng_;
    std::array<uint8_t, 0x30> reg_{};
    uint8_t misc_ = 0;   // last value written to 20015
};

} // namespace tn34xx
