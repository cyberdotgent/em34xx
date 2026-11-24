// SPDX-License-Identifier: GPL-3.0-or-later
// IBM 3477 system gate array, mapped at 20000-200FF.
//
// Registers are 16 bits wide at even addresses; where a register belongs to
// the two Twinax channels the even byte is channel 0 and the odd byte is
// channel 1. The block holds the interrupt controller, a fixed-rate timer,
// configuration straps, the alarm/indicator latch, the keyboard interface,
// the Twinax engine registers and a printer port. See docs/bringup-3477.md
// for the register map and the evidence behind each entry.
#pragma once

#include "bus/device.hpp"
#include "machine/machine.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace em34xx {

class Ibm3477KeyboardInterface;

// Interrupt controller of the gate array. There is no 8259: a 16-bit
// request register (read at 20002, write 1 to clear) and a 16-bit enable
// mask (20004, 1 = enabled) cover the timer and the Twinax sources; the
// display and keyboard have their own request lines. Each source has a fixed
// vector, presented on the interrupt-acknowledge cycle. The 3486 family's
// board has a third Twinax channel and a second serial port, and its
// vectors from 09h on are one or two higher (set_three_channel_layout()).
// Requests are levels:
// a source stays pending until its device or a W1C write clears it.
class Ibm3477Interrupts : public InterruptController {
public:
    // In priority order, highest first.
    enum class Source {
        Timer,
        Twinax0Event, Twinax1Event, Twinax2Event,
        Twinax0Status, Twinax1Status, Twinax2Status,
        Display,
        Aux,        // second serial port (3486 family)
        Keyboard,
    };
    static constexpr int kSourceCount = 10;

    // Request-register bits of the latched sources.
    static constexpr uint16_t kTimerBit = 0x8000;
    static constexpr uint16_t kTwinax0EventBit = 0x0001;
    static constexpr uint16_t kTwinax1EventBit = 0x0100;
    static constexpr uint16_t kTwinax0StatusBits = 0x007E;
    static constexpr uint16_t kTwinax1StatusBits = 0x7E00;

    uint8_t vector(Source s) const { return vectors_[size_t(s)]; }
    // 3486 family [observed: the vectors its firmware installs handlers on]:
    // Twinax events 07-09h and status 0A-0Ch for three channels, display
    // 0Dh, second serial port 0Eh, keyboard 0Fh. The 3477 has events
    // 07/08h, status 09/0Ah, display 0Ch and keyboard 0Eh.
    void set_three_channel_layout();
    static const char* source_name(Source s);

    // Drive a source's request line. For the latched sources raise() sets
    // their request bit (the lowest one for the status groups) and lower()
    // clears all of their bits; Display and Keyboard are plain levels.
    void raise(Source s);
    void lower(Source s);
    // Set specific request-register bits (e.g. one Twinax status bit).
    void request(uint16_t bits) { req_ |= bits; }

    uint16_t requests() const { return req_; }
    void acknowledge_bits(uint16_t bits) { req_ = uint16_t(req_ & ~bits); }
    uint16_t mask() const { return mask_; }
    void set_mask(uint16_t m) { mask_ = m; }
    // Third Twinax channel of the 3486 family: its own request (2000D) and
    // mask (2000F) registers, same bit layout as one byte of 20002/20004.
    void request2(uint8_t bits) { req2_ |= bits; }
    uint8_t requests2() const { return req2_; }
    void acknowledge_bits2(uint8_t bits) { req2_ = uint8_t(req2_ & ~bits); }
    uint8_t mask2() const { return mask2_; }
    void set_mask2(uint8_t m) { mask2_ = m; }
    bool keyboard_enabled() const { return kbd_enable_; }
    void set_keyboard_enabled(bool on) { kbd_enable_ = on; }
    bool aux_enabled() const { return aux_enable_; }
    void set_aux_enabled(bool on) { aux_enable_ = on; }
    bool line(Source s) const;   // request present, ignoring the enables

    // Highest-priority pending and enabled source; false if none.
    bool highest(Source& out) const;

    bool intr_asserted() const override;
    uint8_t acknowledge() override;

    void reset();
    uint64_t delivered(Source s) const { return delivered_[size_t(s)]; }
    std::string describe() const;

private:
    uint16_t req_ = 0;
    uint16_t mask_ = 0;
    uint8_t req2_ = 0;
    uint8_t mask2_ = 0;
    bool kbd_enable_ = false;
    // 3477 layout; the entries of sources it does not have are never used.
    std::array<uint8_t, kSourceCount> vectors_{{0x06, 0x07, 0x08, 0xFF, 0x09, 0x0A, 0xFF, 0x0C, 0xFF, 0x0E}};
    bool three_channel_ = false;
    bool aux_enable_ = false;
    bool display_ = false;
    bool keyboard_ = false;
    bool aux_ = false;
    std::array<uint64_t, kSourceCount> delivered_{};
};

// Twinax link device hook: receives the accesses to the Twinax registers
// (channel control, station addresses, engine control, loopback DMA and
// frame registers) and the writes to 20015, whose loopback bits belong to the
// engine. Without a link device those registers are plain storage. Reads
// must have no side effects.
class Ibm3477TwinaxPort {
public:
    virtual ~Ibm3477TwinaxPort() = default;
    virtual uint8_t twinax_read(uint32_t offset) = 0;
    virtual void twinax_write(uint32_t offset, uint8_t value) = 0;
};

// Strap byte for a display subtype (register 2000A). Hypothesis; see
// docs/bringup-3477.md.
// Straps of the monochrome 3477 this ROM can be (display type 4, FE0).
constexpr uint8_t kIbm3477MonoStraps = 0x0C;
// Paper-white as the firmware knows it (display type 5, FW0): its own cell
// heights and monitor-control setting. Not selected by any variant (the
// tube is a phosphor of the monochrome unit); reachable with --straps 0E.
constexpr uint8_t kIbm3477PaperWhiteStraps = 0x0E;

class Ibm3477System : public Device {
public:
    static constexpr uint32_t kBase = 0x20000;
    static constexpr uint32_t kSize = 0x100;
    static constexpr unsigned kTimerHz = 50;

    // Takes ownership of the keyboard interface.
    Ibm3477System(uint64_t clock_hz, uint8_t straps, std::unique_ptr<Ibm3477KeyboardInterface> kbd);
    ~Ibm3477System() override;

    Ibm3477Interrupts& interrupts() { return ic_; }
    Ibm3477KeyboardInterface& keyboard_interface() { return *kbd_; }
    void set_twinax_port(Ibm3477TwinaxPort* p) { twinax_ = p; }   // hook for the link device
    void set_audio(AudioSink* a) { audio_ = a; }                   // alarm tone and speaker gate
    // 3486 family: a third Twinax channel, with request/mask registers at
    // 2000D/2000F and link registers at 2000B and 2001A/2001B. Off, those are plain
    // storage (3477).
    void set_third_channel(bool on) { third_channel_ = on; }
    uint8_t straps() const { return straps_; }
    bool tone_on() const { return (status_latch_ & 0x80) != 0; }
    bool speaker_gate() const { return (misc_ & 0x02) != 0; }
    uint64_t timer_period_cycles() const { return timer_period_; }

    uint8_t mmio_read8(uint32_t offset) override;
    void mmio_write8(uint32_t offset, uint8_t value) override;
    uint8_t peek8(uint32_t offset) override;
    void reset() override;
    void tick(uint64_t cycles) override;
    std::string describe() const override;

private:
    uint8_t read(uint32_t offset, bool side_effects);
    bool is_twinax_register(uint32_t offset) const;

    Ibm3477Interrupts ic_;
    std::unique_ptr<Ibm3477KeyboardInterface> kbd_;
    Ibm3477TwinaxPort* twinax_ = nullptr;
    AudioSink* audio_ = nullptr;
    bool third_channel_ = false;
    uint8_t straps_;
    uint64_t timer_period_;
    uint64_t timer_acc_ = 0;
    uint64_t timer_ticks_ = 0;

    std::array<uint8_t, kSize> latch_{};   // plain storage for stubbed registers
    uint8_t status_latch_ = 0;             // 20010
    uint8_t misc_ = 0;                     // 20015
};

} // namespace em34xx
