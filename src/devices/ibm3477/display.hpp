// SPDX-License-Identifier: GPL-3.0-or-later
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
//   00-17  CRT timing block, written as one 24-byte table [obs]. Used here:
//          0F = scan lines per row - 1, 11/12 = first/last underline scan
//          line, 14 = rows including the status line, 15 = rows above the
//          status line [hyp, consistent with every mode table]
//   17/18  cursor column / row, 0-based [hyp]
//   21-24  window corners (e.g. 23-24 = 4F 17 for 80x24) [obs values]; not
//          needed for display: the boot screens appear before they are set
//   25-2E  mode block [obs written as a block]:
//          25 bit4 = 132 columns of 8-pixel cells [hyp]
//          2A bit0 = 12-pixel cells, else 16-pixel cells [hyp; matches the
//          width of the font the firmware loads for each mode]
//   28     00 while the font is reloaded [obs]; bit4 video on, bit6 rule
//          above the status line, bit7 cursor on [hyp]
//   2F     W: bit0 SDA pull low, bit1 SCL pull low (I2C master) [obs]
//          R: bit0 SDA low, bit1 SCL low, bit3 vertical retrace [obs],
//             bits 7..4 video signature of the last measured frame [obs
//             protocol]; written bits 6-5 select the source, bit 7 starts a
//             measurement [obs]. The signature function is unknown: see the
//             TEMPORARY VDO shim below and docs/bringup-3477.md.
//   30-31  start offset of the main window, relative to 8000 [obs]
//   34-35  start offset of the status line, relative to 8000 [obs]
//   36     W: acknowledge the display interrupt (vector 0Ch) [obs]
//   38-3B  block request (written 3B..38); completion raises the display
//          interrupt [hyp: completes at the next vertical retrace]
// Cells are 3 bytes (EBCDIC code, attribute, extension) [obs]; rows follow
// each other without gaps [obs]. Glyph g of a font is 32 bytes at g*32, one
// byte per scan line, bit 7 leftmost [obs]; font 1 at 18000 (+ extension
// columns at 1A000), font 2 at 1C000 (+ 1E000) [obs]. See display.cpp for
// the attribute decoding.
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

namespace em34xx {

class I2cBus;
class Ibm3486AuxPort;

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
    // 3486 family: the second serial port's registers at 20144-20146 sit in
    // this page. Without it they are plain storage.
    void set_aux_port(Ibm3486AuxPort* port) { aux_ = port; }
    // CPU clock used to derive frame timing (default 8 MHz).
    void set_clock_hz(uint64_t hz);
    // Vertical retrace lasts 1/divisor of the frame (default 90, which the
    // 3477's display test accepts). Other tubes need another length.
    void set_retrace_divisor(unsigned divisor);
    // 3486 family: 20128 bit 3 inverts the retrace flag read at 2012F bit 3.
    void set_sync_polarity_bit(bool on) { sync_polarity_bit_ = on; }
    // Position of the 2-bit signature selector in 2012F: bits 6-5 on the
    // 3477 (shift 5), bits 5-4 on the 3486 family (shift 4) [observed: the
    // values each firmware writes].
    void set_signature_selector_shift(unsigned shift) { sig_selector_shift_ = shift; }
    // 3486 family: the picture is on while 20128 bit 6 is set, not bit 4
    // [hypothesis; see enabled()].
    void set_video_enable_bit6(bool on) { video_enable_bit6_ = on; }
    // Value of 2012F bits 7..4 when no signature result is latched.
    void set_sense(uint8_t nibble) { sense_ = uint8_t(nibble & 0x0F); }

    // --- TEMPORARY: video signature shim (power-on display test, VDO) ---
    // The real hardware folds one frame of video into a 4-bit signature
    // whose function is unknown (docs/bringup-3477.md, "Display test").
    // Until it is measured, the shim stands in for it: a measurement (2012F
    // bit 7 set) ends at the next start of vertical retrace and latches a
    // nibble looked up from the selector (bits 6-5) and a fingerprint of the
    // frame the controller displayed (vdo_fingerprint()), in a small table of
    // results measured for the test screens. Unknown frames get a
    // placeholder nibble. It never looks at the CPU or firmware state.
    // On by default; TerminalOptions::vdo_shim = false (--no-vdo-shim)
    // turns it off, and bits 7..4 then read the sense nibble (0).
    void set_vdo_shim(bool on) { vdo_shim_ = on; }
    bool vdo_shim() const { return vdo_shim_; }
    // Fingerprint (64-bit FNV-1a) of what the controller shows: its timing,
    // mode, cursor and window registers and, for every displayed cell, the
    // cell bytes and the glyph scan lines it fetches.
    uint64_t vdo_fingerprint() const;
    // The shim's result for a selector (0-3) and the current frame.
    uint8_t vdo_shim_signature(int selector) const;

    // --- Device ------------------------------------------------------
    uint8_t mmio_read8(uint32_t offset) override;
    void mmio_write8(uint32_t offset, uint8_t value) override;
    uint8_t peek8(uint32_t offset) override;
    void reset() override;
    void tick(uint64_t cycles) override;
    std::string describe() const override;

    // --- DisplaySource -------------------------------------------------
    void render(Framebuffer& fb) override;
    uint64_t animation_phase() const override;   // change tracking: the blink phase

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
    uint8_t mem(uint32_t addr) const;

    Bus& bus_;
    TerminalOptions opts_;
    const Region* ram_ = nullptr;   // RAM region cached during render()
    int last_w_ = 0, last_h_ = 0;
    std::array<uint8_t, 256> regs_{};
    std::function<void(bool)> irq_;
    I2cBus* i2c_ = nullptr;
    Ibm3486AuxPort* aux_ = nullptr;
    uint8_t sense_ = 0;
    uint64_t clock_hz_ = 8000000;
    uint64_t frame_cycles_ = 0;
    uint64_t retrace_cycles_ = 0;
    unsigned retrace_divisor_ = 90;
    bool sync_polarity_bit_ = false;
    unsigned sig_selector_shift_ = 5;
    bool video_enable_bit6_ = false;
    uint64_t phase_ = 0;
    uint64_t frames_ = 0;
    bool in_retrace_ = false;
    bool irq_pending_ = false;
    bool block_pending_ = false;
    // Signature measurement (see the TEMPORARY shim above).
    bool vdo_shim_ = true;
    bool sig_measuring_ = false;
    bool sig_valid_ = false;
    uint8_t sig_result_ = 0;
};

} // namespace em34xx
