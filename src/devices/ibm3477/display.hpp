// IBM 3477 display controller (registers at 20100-2013F, mapped as the whole
// 256-byte page 20100-201FF).
//
// The controller scans a character window in system RAM (3-byte cells:
// EBCDIC code, attribute, extended attribute) and draws each glyph from
// character-generator RAM at 18000-1FFFF, which the firmware fills at run
// time. The emulator therefore carries no font of its own.
//
// Register map (offsets from 20100). Labels: [obs] observed in firmware
// behaviour, [hyp] hypothesis.
//   00-17  CRT timing block, written as one 24-byte table [obs];
//          interpretation of the individual bytes [hyp]
//   17-1A  cursor shape / position [hyp]
//   21-22  status-line window: top-left (col, row) [hyp]
//   23-24  main window: bottom-right (col, row), e.g. 4F 17 = 80x24 [obs values]
//   25-2E  display mode block [obs written as a block]; 28 = enable/blanking [hyp]
//   2F     W: bit0 SDA pull low, bit1 SCL pull low (I2C master) [obs]
//          R: bit0 SDA low, bit1 SCL low, bit3 vertical retrace [obs],
//             bits 7..4 monitor sense nibble [obs, value hyp]
//   30-31  start offset of the main window, relative to 8000 [obs]
//   34-35  start offset of the status line, relative to 8000 [obs]
//   36     W: acknowledge the display interrupt (vector 0Ch) [obs]
// Everything else latches written values and reads them back.
#pragma once

#include "bus/bus.hpp"
#include "bus/device.hpp"
#include "machine/machine.hpp"
#include "machine/peripherals.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace tn34xx {

class I2cBus;

class Ibm3477Display : public Device, public DisplaySource {
public:
    static constexpr uint32_t kRegBase = 0x20100;
    static constexpr uint32_t kWindowBase = 0x8000;   // cell offsets are relative to this
    static constexpr uint32_t kCharGenBase = 0x18000; // four 8 KiB planes

    Ibm3477Display(Bus& bus, const TerminalOptions& opts);

    // Display interrupt line (vector 0Ch); raised once per frame at the start
    // of vertical retrace, lowered by a write to 20136.
    void set_irq(std::function<void(bool)> line);
    // I2C bus driven by the bit-bang lines in 2012F. May be null.
    void set_i2c(I2cBus* bus);
    // CPU clock used to derive frame timing (default 8 MHz).
    void set_clock_hz(uint64_t hz);
    // Monitor sense nibble presented in 2012F bits 7..4.
    void set_sense(uint8_t nibble) { sense_ = uint8_t(nibble & 0x0F); }

    // --- Device ------------------------------------------------------
    uint8_t mmio_read8(uint32_t offset) override;
    void mmio_write8(uint32_t offset, uint8_t value) override;
    uint8_t peek8(uint32_t offset) override;
    void reset() override;
    void tick(uint64_t cycles) override;
    std::string describe() const override;

    // --- DisplaySource -------------------------------------------------
    void render(Framebuffer& fb) override;

    // --- state for tests and the monitor --------------------------------
    bool irq_pending() const { return irq_pending_; }
    bool in_retrace() const { return in_retrace_; }
    uint64_t frame_count() const { return frames_; }
    uint64_t cycles_per_frame() const { return frame_cycles_; }

    struct Geometry {
        int cols = 0, rows = 0;            // main window
        int cell_w = 0, cell_h = 0;        // pixels
        int status_rows = 0;               // 0 or 1
        uint32_t main_start = 0;           // absolute address of first cell
        uint32_t status_start = 0;
    };
    Geometry geometry() const;
    bool enabled() const;

private:
    uint8_t read_status() const;
    void recompute_timing();
    void set_irq_line(bool on);
    void draw_row(Framebuffer& fb, uint32_t addr, int y0, int cols, int cell_w, int cell_h,
                  int cursor_col, bool blink_phase);

    Bus& bus_;
    const TerminalOptions& opts_;
    std::array<uint8_t, 256> regs_{};
    std::function<void(bool)> irq_;
    I2cBus* i2c_ = nullptr;
    uint8_t sense_ = 0;
    uint64_t clock_hz_ = 8000000;
    uint64_t frame_cycles_ = 0;
    uint64_t retrace_cycles_ = 0;
    uint64_t phase_ = 0;
    uint64_t frames_ = 0;
    bool in_retrace_ = false;
    bool irq_pending_ = false;
};

} // namespace tn34xx
