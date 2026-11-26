// SPDX-License-Identifier: GPL-3.0-or-later
// The devices of the IBM 3483 logic board (06G5500), as the firmware
// 06G5525 uses them. Everything here comes from the firmware's own use of
// the registers (observed in code, analysis notes) unless marked
// hypothesis; no board documentation is available.
//
//  58000  display controller: 6845-like timing pairs +00..+0E, cursor and
//         control registers, start address +1C (the regen buffer, 0:1000
//         after POST), status/control +26, palette +40..+6F.
//  58100  coax adapter and the board's two interrupt status registers
//         (+0C and +20, write 1 to clear).
//  58400  system control: indicators, two serial receive channels
//         (+06/+08, +10/+12) and the sense register +0E (POST mode,
//         vertical sync, monitor sense lines).
//
// The ASIC presents its interrupts on the 80186's INT0 in cascade mode and
// supplies the vector on INTA; the firmware installs handlers at 20h and
// 21h only, so the model supplies 20h [hypothesis].
#pragma once

#include "bus/device.hpp"
#include "machine/peripherals.hpp"

#include <array>
#include <memory>
#include <cstdint>
#include <functional>
#include <string>

namespace em34xx {

class Bus;
class I80186Peripherals;
class Set3Keyboard;

class Board3483 : public InterruptController {
public:
    // `monitor_sense`: the code the four monitor sense lines give, as the
    // firmware packs it (2 bits per line, line 1 in bits 7-6: 00 grounded,
    // 01 open, 10 follows 58026 bit 5, 11 follows bit 0). 0x51 = type 1.
    Board3483(Bus& bus, I80186Peripherals& cpu_io, uint64_t clock_hz, uint8_t monitor_sense);

    class Display;
    class Coax;
    class System;
    class Window;
    Display& display() { return *display_; }
    Coax& coax() { return *coax_; }
    System& system() { return *system_; }

    // Frame timing: true while vertical sync is active.
    bool in_vsync() const { return frame_pos_ >= active_cycles_; }
    void advance(uint64_t cycles);

    // Interrupt status (W1C registers 5810C and 58120) and the INT0 line.
    void raise1(uint16_t bits);
    void raise2(uint16_t bits);
    void update_irq();

    // InterruptController: the cascade "slave" on INT0.
    bool intr_asserted() const override;
    uint8_t acknowledge() override { return 0x20; }

private:
    friend class Display;
    friend class Coax;
    friend class System;

    Bus& bus_;
    I80186Peripherals& cpu_io_;
    Display* display_;
    Coax* coax_;
    System* system_;
    Window* window_ = nullptr;
    bool window_mapped_ = false;
    void set_window(bool on);
    uint8_t monitor_sense_;
    uint64_t frame_cycles_;
    uint64_t active_cycles_;
    uint64_t frame_pos_ = 0;
    uint64_t frame_count_ = 0;
    uint16_t status1_ = 0, status2_ = 0;   // 5810C, 58120
};

class Board3483::Display : public Device, public DisplaySource {
public:
    explicit Display(Board3483& board);
    uint8_t mmio_read8(uint32_t offset) override;
    void mmio_write8(uint32_t offset, uint8_t value) override;
    uint16_t mmio_read16(uint32_t offset) override;
    void mmio_write16(uint32_t offset, uint16_t value) override;
    uint8_t peek8(uint32_t offset) override;
    void reset() override;
    void tick(uint64_t cycles) override { board_.advance(cycles); }
    std::string describe() const override;

    void render(Framebuffer& fb) override;
    uint64_t animation_phase() const override { return board_.frame_count_ / 32; }

    uint16_t reg(unsigned offset) const { return regs_[(offset & 0xFF) >> 1]; }
    // A colour tube shows the three guns; a monochrome one their level.
    void set_colour(bool colour) { colour_ = colour; }

private:
    uint16_t read_word(unsigned offset) const;
    void write_word(unsigned offset, uint16_t value);
    bool colour_ = false;
    struct FieldState;
    uint16_t cell_dots(uint8_t ch, uint8_t eab, FieldState& field, unsigned y, unsigned cell_h,
                       unsigned& index) const;
    FieldState initial_field() const;
    unsigned cell_height() const;
    unsigned lit_guns() const;
    uint8_t level(unsigned gun, unsigned index) const;
    unsigned rows() const;
    unsigned columns() const;
    uint32_t row_address(unsigned row) const;

    Board3483& board_;
    std::array<uint16_t, 128> regs_{};
};

// While 58021 bit 7 is set, 40000-47FFF is a write window [observed: POST
// test 4, C000:A674]: a word written at 40000+n puts its low byte into both
// bytes at 40000+n and its high byte into both bytes at 48000+n (of which
// the test checks the low nibble). Reads pass through. What the display
// does with the two planes is not known yet [hypothesis: character and
// attribute planes of the character generator].
class Board3483::Window : public Device {
public:
    explicit Window(Board3483& board) : Device("window-3483"), board_(board) { board_.window_ = this; }
    uint8_t mmio_read8(uint32_t offset) override;
    void mmio_write8(uint32_t offset, uint8_t value) override;
    void mmio_write16(uint32_t offset, uint16_t value) override;
    uint8_t peek8(uint32_t offset) override { return mmio_read8(offset); }

private:
    uint8_t* sram(uint32_t offset);
    Board3483& board_;
};

class Board3483::Coax : public Device {
public:
    explicit Coax(Board3483& board);
    uint8_t mmio_read8(uint32_t offset) override;
    void mmio_write8(uint32_t offset, uint8_t value) override;
    uint16_t mmio_read16(uint32_t offset) override;
    void mmio_write16(uint32_t offset, uint16_t value) override;
    uint8_t peek8(uint32_t offset) override;
    void reset() override;
    std::string describe() const override;
    uint16_t reg(unsigned offset) const { return regs_[(offset & 0xFF) >> 1]; }

private:
    uint16_t read_word(unsigned offset, bool side_effects);
    void write_word(unsigned offset, uint16_t value);
    void transmit(uint16_t word);
    void run_engine(uint16_t command);
    uint32_t cell_address(uint16_t counter) const;
    void store(uint16_t counter, uint8_t data);
    uint8_t fetch(uint16_t counter) const;

    Board3483& board_;
    std::array<uint16_t, 128> regs_{};
    uint16_t addr_ = 0, addr2_ = 0;   // address counters (+04, +06)
    uint16_t rx_ = 0;                 // +08
    uint16_t tx_ = 0;                 // word written to +0A
    bool lac_hi_ = false;
    uint8_t lac_target_ = 0;          // which counter a LOAD ADDRESS COUNTER pair loads
    uint16_t pending_cmd_ = 0;        // last command word, for the data word after it
    uint8_t mask_ = 0;                // LOAD MASK: bits CLEAR and WRITE DATA leave alone
};

class Board3483::System : public Device {
public:
    explicit System(Board3483& board);
    uint8_t mmio_read8(uint32_t offset) override;
    void mmio_write8(uint32_t offset, uint8_t value) override;
    uint16_t mmio_read16(uint32_t offset) override;
    void mmio_write16(uint32_t offset, uint16_t value) override;
    uint8_t peek8(uint32_t offset) override;
    void reset() override;
    std::string describe() const override;

    // A byte from the device on serial channel A (keyboard, hypothesis) or
    // B: latched with odd parity in bit 8, and the channel's interrupt
    // raised when it is armed.
    bool receive(unsigned channel, uint8_t byte);
    // The keyboard on channel A (the 122-key terminal keyboard: POST
    // expects ID BF BF) [hypothesis for the channel].
    void attach_keyboard(Set3Keyboard* kbd, uint64_t clock_hz);
    void tick(uint64_t cycles) override;

private:
    uint16_t read_word(unsigned offset) const;
    void write_word(unsigned offset, uint16_t value);
    uint16_t sense() const;

    Board3483& board_;
    std::array<uint16_t, 128> regs_{};
    struct Channel {
        bool armed = false;
        uint16_t data = 0xFFFF;
        uint16_t control = 0;
        uint16_t drive = 0xFFFF;  // last word written to the data register
        uint16_t shift = 0;      // frame bits received from the lines so far
        unsigned bits = 0;
    };
    void control_write(unsigned channel, uint16_t value);
    void data_write(unsigned channel, uint16_t value);
    Set3Keyboard* kbd_ = nullptr;
    uint64_t clock_hz_ = 1;
    uint64_t byte_cycles_ = 0;
    uint64_t us_acc_ = 0;
    uint64_t tx_left_ = 0;    // cycles until the byte being sent reaches the keyboard
    uint8_t tx_byte_ = 0;
    uint64_t rx_left_ = 0;    // cycles until the keyboard's next byte arrives
    bool rx_active_ = false;
    uint64_t hold_left_ = 0;  // the keyboard is still busy with a command
    void frame_received(unsigned channel, uint16_t frame);
    std::array<Channel, 2> channels_{};
};

} // namespace em34xx
