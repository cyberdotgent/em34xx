// SPDX-License-Identifier: GPL-3.0-or-later
// The devices of the IBM 3483 logic board (06G5500), as the firmware
// 06G5525 uses them. Everything here comes from the firmware's own use of
// the registers (observed in code, analysis notes) unless marked
// hypothesis; no board documentation is available.
//
//  58000  display controller: 6845-like timing pairs +00..+0E (columns,
//         rows, cell lines), cursor position +10, rule line +12, cursor
//         lines and underscore style +16, control +1A/+1C/+1E (linear or
//         row-list screen, cursor on/block/blink, rule style), status and
//         control +26, palette +40..+6F.
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
#include "machine/coax_port.hpp"
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

// Where the board's parts sit. The 3472's gate array has the same
// registers at other addresses [observed in its firmware]; the defaults
// are the 3483's.
struct CoaxBoardLayout {
    uint32_t display = 0x58000, coax = 0x58100, system = 0x58400;
    // Battery-backed SRAM: the write window at its base, the base and
    // APL/GE character sets at +8000/+A000, their companion planes 8000
    // below.
    uint32_t sram = 0x40000;
    // Block RAM: the row lists at +108/+508, the coax registers at +300.
    uint32_t block_ram = 0x50000;
};

class Board3483 : public InterruptController {
public:
    using Layout = CoaxBoardLayout;

    // `monitor_sense`: the code the four monitor sense lines give, as the
    // firmware packs it (2 bits per line, line 1 in bits 7-6: 00 grounded,
    // 01 open, 10 follows 58026 bit 5, 11 follows bit 0). 0x51 = type 1.
    // `cpu_io`: the 80186's integrated peripherals, whose INT0 the board
    // drives in cascade mode; null for a plain 8086, which takes the board
    // itself as its interrupt controller (Bus::set_interrupt_controller).
    Board3483(Bus& bus, I80186Peripherals* cpu_io, uint64_t clock_hz, uint8_t monitor_sense, Layout layout = {});
    const Layout& layout() const { return layout_; }

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
    I80186Peripherals* cpu_io_;
    Layout layout_;
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
    void draw(Framebuffer& fb);   // the raster at one pixel per dot
    uint64_t animation_phase() const override { return board_.frame_count_ / 32; }

    uint16_t reg(unsigned offset) const { return regs_[(offset & 0xFF) >> 1]; }

    // The geometry the mode registers give (see board_3483.cpp).
    unsigned rows() const;
    unsigned columns() const;
    unsigned cell_height() const;

    // The hardware cursor as 58010, 5801C and 58016 describe it.
    struct Cursor {
        bool visible = false;
        bool block = false;
        bool blink = false;
        unsigned row = 0, column = 0;
        unsigned first_line = 0, last_line = 0;   // underline cursor, from 0
    };
    Cursor cursor() const;

private:
    uint16_t read_word(unsigned offset) const;
    void write_word(unsigned offset, uint16_t value);
    struct FieldState;
    uint16_t cell_dots(uint8_t ch, uint8_t eab, FieldState& field, unsigned y, unsigned cell_h,
                       unsigned& index, bool blink_off) const;
    unsigned cell_colour(uint8_t ch, uint8_t eab, const FieldState& field) const;
    bool on_underline(unsigned y, unsigned cell_h) const;
    uint16_t underscore_dots() const;
    bool blink_off() const;
    FieldState initial_field() const;
    unsigned lit_guns() const;
    uint8_t level(unsigned gun, unsigned index) const;
    uint32_t row_address(unsigned row) const;

    Board3483& board_;
    std::array<uint16_t, 128> regs_{};
};

// While 58021 bit 7 is set, 40000-47FFF is a write window into the
// character generator, one word per glyph row: the word at 40000+w goes
// to row w/2, its low byte to 40000+w/2 and its high byte to 48000+w/2.
// [observed: POST test 4 (C000:A674) writes 20h words of 0A5A at window
// offsets 0-3E and expects 20h bytes of 5A at 40000 and 20h bytes with low
// nibble A at 48000; the display test (E000:1392) addresses character c,
// row r at window offset c*64 + r*2.] Reads pass through [hypothesis].
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

// The coax adapter (58100-5812F) with the board's two interrupt status
// registers; see board_3483_coax.cpp. It decodes and answers the
// controller's commands itself, as the firmware leaves it to (link mode),
// and loops the firmware's own words through the same decoder in POST's
// wrap modes.
class Board3483::Coax : public Device {
public:
    explicit Coax(Board3483& board);
    uint8_t mmio_read8(uint32_t offset) override;
    void mmio_write8(uint32_t offset, uint8_t value) override;
    uint16_t mmio_read16(uint32_t offset) override;
    void mmio_write16(uint32_t offset, uint16_t value) override;
    uint8_t peek8(uint32_t offset) override;
    void reset() override;
    void tick(uint64_t cycles) override;
    std::string describe() const override;
    uint16_t reg(unsigned offset) const { return regs_[(offset & 0xFF) >> 1]; }

    // The controller on the line (the machine's CoaxHost), looked up on
    // every use: a connect swaps it.
    void set_line(std::function<CoaxHost*()> line) { line_ = std::move(line); }

    // One transmission from the controller: the reply words, or false when
    // the terminal does not answer (an undefined command). Public for tests.
    bool execute(const CoaxMessage& msg, CoaxMessage& reply);

private:
    uint16_t read_word(unsigned offset, bool side_effects);
    void write_word(unsigned offset, uint16_t value);
    void run_engine(uint16_t command);

    // The decoder, one word at a time: begin() takes a command word,
    // data() each data word after it; reply_ holds the words answered so far.
    void begin(uint8_t cmd);
    void data(uint8_t byte);
    bool defined(uint8_t cmd) const;
    uint16_t& counter(unsigned which) { return which == 2 ? addr2_ : addr_; }
    uint32_t cell(unsigned which, uint16_t at) const;
    uint8_t reg_byte(unsigned index) const;
    void reply_byte(uint8_t byte);
    void reply_poll();
    // The adapter register's 16-bit view of a coax word (5810A / 58108).
    static uint16_t to_register(CoaxWord w);
    static CoaxWord from_register(uint16_t r);
    void wrap_transmit(uint16_t reg_word);

    Board3483& board_;
    std::function<CoaxHost*()> line_;
    std::array<uint16_t, 128> regs_{};
    uint16_t addr_ = 0, addr2_ = 0;   // counter 1 (13 bits, display), counter 2 (12 bits, printer)
    uint16_t rx_ = 0, tx_ = 0;        // 58108, the word written to 5810A
    uint8_t mask_ = 0, eab_mask_ = 0, mask2_ = 0;
    uint8_t cmd_ = 0;                 // the command being executed
    unsigned data_words_ = 0;         // data words received after it
    bool waiting_ = false;            // a command that takes data words
    bool armed_ = false;              // a poll response is armed (58102 write, bit 7)
    bool alarm_ = false, click_ = false;
    uint8_t eab_pending_ = 0;         // WRITE ALTERNATE: char byte awaiting its EAB byte
    bool undefined_ = false;          // the command is undefined: no reply
    CoaxMessage reply_;
    // Line timing.
    uint64_t busy_ = 0;               // cycles until the line is idle again
    uint64_t ns_acc_ = 0, emu_ns_ = 0;
    uint64_t timer_left_ = 0;         // cycles to the next periodic tick (5810C bit 6)
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
    // Where the alarm and the clicker go (58402).
    void set_audio(AudioSink* audio) { audio_ = audio; }

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
    void alarm_write(uint16_t value, uint16_t old);
    AudioSink* audio_ = nullptr;
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
