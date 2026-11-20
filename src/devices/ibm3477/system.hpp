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

namespace tn34xx {

class Ibm3477KeyboardInterface;

// Interrupt controller of the gate array. There is no 8259: a 16-bit
// request register (read at 20002, write 1 to clear) and a 16-bit enable
// mask (20004, 1 = enabled) cover the timer and the Twinax sources; the
// display and keyboard have their own request lines. Each source has a fixed
// vector, presented on the interrupt-acknowledge cycle. Requests are levels:
// a source stays pending until its device or a W1C write clears it.
class Ibm3477Interrupts : public InterruptController {
public:
    // In priority order, highest first.
    enum class Source { Timer, Twinax0Event, Twinax1Event, Twinax0Status, Twinax1Status, Display, Keyboard };
    static constexpr int kSourceCount = 7;

    // Request-register bits of the latched sources.
    static constexpr uint16_t kTimerBit = 0x8000;
    static constexpr uint16_t kTwinax0EventBit = 0x0001;
    static constexpr uint16_t kTwinax1EventBit = 0x0100;
    static constexpr uint16_t kTwinax0StatusBits = 0x007E;
    static constexpr uint16_t kTwinax1StatusBits = 0x7E00;

    static uint8_t vector(Source s);
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
    bool keyboard_enabled() const { return kbd_enable_; }
    void set_keyboard_enabled(bool on) { kbd_enable_ = on; }
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
    bool kbd_enable_ = false;
    bool display_ = false;
    bool keyboard_ = false;
    std::array<uint64_t, kSourceCount> delivered_{};
};

// Future Twinax link device: receives the accesses to the Twinax registers
// (channel command/status, configuration, DMA and frame registers). Until it
// exists those registers are plain storage.
class Ibm3477TwinaxPort {
public:
    virtual ~Ibm3477TwinaxPort() = default;
    virtual uint8_t twinax_read(uint32_t offset) = 0;
    virtual void twinax_write(uint32_t offset, uint8_t value) = 0;
};

// Strap byte for a display subtype (register 2000A). Hypothesis; see
// docs/bringup-3477.md.
uint8_t ibm3477_default_straps(DisplaySubtype d);

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
    static bool is_twinax_register(uint32_t offset);

    Ibm3477Interrupts ic_;
    std::unique_ptr<Ibm3477KeyboardInterface> kbd_;
    Ibm3477TwinaxPort* twinax_ = nullptr;
    uint8_t straps_;
    uint64_t timer_period_;
    uint64_t timer_acc_ = 0;
    uint64_t timer_ticks_ = 0;

    std::array<uint8_t, kSize> latch_{};   // plain storage for stubbed registers
    uint8_t status_latch_ = 0;             // 20010
    uint8_t misc_ = 0;                     // 20015
};

} // namespace tn34xx
