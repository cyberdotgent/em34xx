// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/display_controller.hpp"

#include "hw/aux_port.hpp"

#include "hw/i2c.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>

namespace em34xx {

namespace {
constexpr double kDisplayAspect = 4.0 / 3.0;
// Black margins around the raster, as a fraction of the visible tube on each
// side. Measured on photographs of a real 3486 tube showing an 80x24+1
// screen (same display design): the raster covers ~88% of the glass width
// and ~85% of its height, so it is wider than 4:3 (about 1.38).
constexpr double kDisplayBorderX = 0.06;
constexpr double kDisplayBorderY = 0.077;
}

namespace {

// Register offsets within the 20100 page. See the header for the labels.
constexpr uint32_t kRegCellLines = 0x0F;   // scan lines per row - 1
constexpr uint32_t kRegLineTop = 0x11;     // first underline/cursor scan line
constexpr uint32_t kRegLineBottom = 0x12;  // last underline/cursor scan line
constexpr uint32_t kRegTotalRows = 0x14;   // rows including the status line
constexpr uint32_t kRegMainRows = 0x15;    // rows above the status line
constexpr uint32_t kRegCursorCol = 0x17;
constexpr uint32_t kRegCursorRow = 0x18;
constexpr uint32_t kRegMode0 = 0x25;       // bit4: 132 columns of 8-pixel cells
constexpr uint32_t kRegControl = 0x28;     // bit4 video on, bit6 status rule, bit7 cursor
constexpr uint32_t kRegDim = 0x29;         // bit7: data area blanked (auto dim)
constexpr uint32_t kRegMode5 = 0x2A;       // bit0: 12-pixel cells, else 16
constexpr uint32_t kRegStatus = 0x2F;
constexpr uint32_t kRegStart = 0x30;
constexpr uint32_t kRegStatusStart = 0x34;
constexpr uint32_t kRegIrqAck = 0x36;
constexpr uint32_t kRegBlockStart = 0x38;  // block request; written 3B, 3A, 39, 38
constexpr uint32_t kRegAuxPort = 0x44;     // 3486 family: second serial port, 44-46

constexpr uint8_t kCtlVideo = 0x10;
constexpr uint8_t kCtlStatusRule = 0x40;
constexpr uint8_t kCtlCursor = 0x80;
constexpr uint8_t kDimData = 0x80;

constexpr uint64_t kRefreshHz = 60;
constexpr uint32_t kBlinkFrames = 32;      // half period of character blink

constexpr uint32_t kBlack = 0xFF000000u;
// 5250 colour set.
constexpr uint32_t kGreen = 0xFF20E040u;
constexpr uint32_t kWhite = 0xFFF0F0F0u;
constexpr uint32_t kRed = 0xFFF03030u;
constexpr uint32_t kTurquoise = 0xFF30E0E0u;
constexpr uint32_t kYellow = 0xFFF0F040u;
constexpr uint32_t kPink = 0xFFF070D0u;
constexpr uint32_t kBlue = 0xFF5878FFu;
// Monochrome intensities as grey levels; the display profile tints them.
// Normal intensity is close to full brightness on a real tube (photos).
constexpr uint32_t kGreyNormal = 0xFFD0D0D0u;
constexpr uint32_t kGreyBright = 0xFFFFFFFFu;

struct CellLook {
    uint32_t fg = kGreyNormal;
    bool reverse = false;
    bool underline = false;
    bool blink = false;
    bool hidden = false;
    bool column_sep = false;
    bool font2 = false;
};

// A cell's attribute and extension bytes.
// Attribute, all units: bit 7 selects font 2 [obs]; bit0 reverse, bit2
// underline, bit4 column separators [obs on host screens]; bit3 blink [hyp].
// Monochrome units: bit1 high intensity [obs on host screens].
// Colour units: one bit per gun, in the attribute or the extension byte.
// Non-display is one bit as well. Where these are differs per board
// (DisplayLayout::Cells). No gun at all has not been seen; it is drawn
// green rather than black so that such text cannot vanish [hyp].
CellLook decode_attr(uint8_t attr, uint8_t ext, bool colour, const DisplayLayout::Cells& cc) {
    CellLook l;
    l.font2 = attr & 0x80;
    l.reverse = attr & 0x01;
    l.underline = attr & 0x04;
    l.blink = attr & 0x08;
    l.column_sep = attr & 0x10;
    l.hidden = (cc.hidden_byte == 2 ? ext : attr) & cc.hidden;
    if (!colour) {
        l.fg = (attr & 0x02) ? kGreyBright : kGreyNormal;
        return l;
    }
    const uint8_t guns = cc.gun_byte == 2 ? ext : attr;
    static constexpr uint32_t kGuns[8] = {kGreen, kRed, kGreen, kYellow, kBlue, kPink, kTurquoise, kWhite};
    l.fg = kGuns[((guns & cc.red) ? 1 : 0) | ((guns & cc.green) ? 2 : 0) | ((guns & cc.blue) ? 4 : 0)];
    return l;
}

} // namespace

DisplayController::DisplayController(Bus& bus, const DisplayLayout& layout, bool colour, bool vdo_shim)
    : Device("display"), bus_(bus), colour_(colour), layout_(layout), vdo_shim_(vdo_shim) {
    if (!layout_.retrace_divisor) layout_.retrace_divisor = 90;
    // Display change tracking (Machine::take_display_change): the cell
    // windows (offsets from 8000 are 16-bit [hyp], so 8000-17FFF), the
    // character generator (18000-1FFFF) and the display registers. Done
    // here so that every model that has this display gets it.
    bus_.observe_writes(kWindowBase, 0x20000 - kWindowBase);
    bus_.observe_writes(kRegBase, 0x100);
    reset();
}

void DisplayController::set_irq(std::function<void(bool)> line) { irq_ = std::move(line); }
void DisplayController::set_i2c(I2cBus* bus) { i2c_ = bus; }

void DisplayController::set_clock_hz(uint64_t hz) {
    clock_hz_ = hz ? hz : 8000000;
    recompute_timing();
}

void DisplayController::reset() {
    regs_.fill(0);
    phase_ = 0;
    frames_ = 0;
    in_retrace_ = false;
    block_pending_ = false;
    sig_measuring_ = false;
    sig_valid_ = false;
    sig_result_ = 0;
    set_irq_line(false);
    recompute_timing();
}

void DisplayController::recompute_timing() {
    frame_cycles_ = std::max<uint64_t>(clock_hz_ / kRefreshHz, 100);
    // Vertical sync of a few scan lines out of a ~525-line frame [hyp].
    retrace_cycles_ = std::max<uint64_t>(frame_cycles_ / layout_.retrace_divisor, 1);
    phase_ %= frame_cycles_;
}

void DisplayController::set_irq_line(bool on) {
    if (irq_pending_ == on) return;
    irq_pending_ = on;
    if (irq_) irq_(on);
}

void DisplayController::tick(uint64_t cycles) {
    phase_ += cycles;
    bool frame_started = false;
    while (phase_ >= frame_cycles_) {
        phase_ -= frame_cycles_;
        ++frames_;
        frame_started = true;
    }
    // Each frame begins with vertical retrace.
    const bool retrace = phase_ < retrace_cycles_;
    if (frame_started && sig_measuring_) {
        // A signature measurement covers the field up to the start of the
        // next vertical retrace, where the result is latched [obs protocol].
        sig_measuring_ = false;
        sig_valid_ = true;
        sig_result_ = vdo_shim_signature((regs_[kRegStatus] >> layout_.signature_selector_shift) & 3);
    }
    if ((frame_started || (retrace && !in_retrace_)) && block_pending_) {
        // A block request completes at the next vertical retrace [hyp].
        block_pending_ = false;
        set_irq_line(true);
    }
    in_retrace_ = retrace;
}

uint8_t DisplayController::read_status() const {
    const uint8_t w = regs_[kRegStatus];
    uint8_t v = uint8_t((sig_valid_ ? sig_result_ : sense_) << 4);
    if ((w & 0x01) || (i2c_ && !i2c_->sda_in())) v |= 0x01;
    if (w & 0x02) v |= 0x02;
    // Bit 3 is the vertical sync line. On the 3486 family 20128 bit 3
    // inverts it [observed: the firmware expects the flag inverted on the
    // tubes for which it sets that bit; hypothesis: sync polarity].
    const bool invert = (regs_[layout_.sync_invert.reg] & layout_.sync_invert.mask) != 0;
    if (in_retrace_ != invert) v |= 0x08;
    return v;
}

uint8_t DisplayController::mmio_read8(uint32_t offset) {
    offset &= 0xFF;
    if (offset == kRegStatus) return read_status();
    if (aux_ && offset >= kRegAuxPort && offset < kRegAuxPort + 3) return aux_->read(offset - kRegAuxPort);
    return regs_[offset];
}

uint8_t DisplayController::peek8(uint32_t offset) { return mmio_read8(offset); }

void DisplayController::mmio_write8(uint32_t offset, uint8_t value) {
    offset &= 0xFF;
    const uint8_t old = regs_[offset];
    regs_[offset] = value;
    if (aux_ && offset >= kRegAuxPort && offset < kRegAuxPort + 3) {
        aux_->write(offset - kRegAuxPort, value);
        return;
    }
    switch (offset) {
    case kRegStatus:
        // Bit 7 starts a signature measurement on its rising edge; clearing
        // it discards the result [obs protocol]. Only the shim measures.
        if (!(value & 0x80)) {
            sig_measuring_ = false;
            sig_valid_ = false;
        } else if (!(old & 0x80) && vdo_shim_) {
            sig_measuring_ = true;
            sig_valid_ = false;
        }
        // Open-drain I2C master: a 1 bit pulls the line low.
        if (i2c_) i2c_->set_lines(!(value & 0x02), !(value & 0x01));
        break;
    case kRegIrqAck:
        set_irq_line(false);
        break;
    case kRegBlockStart:
        block_pending_ = true;
        break;
    default:
        break;
    }
}

bool DisplayController::enabled() const {
    // 20128 is 00 while the firmware reloads the character generator [obs];
    // bit 4 is set whenever a screen is meant to be visible [hyp].
    // 3486 family: 20128 bits 4-3 are constants of the tube (10 on type 2,
    // 18 on types 3-5, 00 on colour) [observed], so bit 4 is not "video on"
    // there. The firmware writes the register without bit 6 while it changes
    // the mode and with it when the screen is ready [observed], so bit 6
    // stands in for it [hypothesis].
    const bool video = (regs_[layout_.video_on.reg] & layout_.video_on.mask) != 0;
    return video && regs_[kRegMainRows] <= 64 &&
           regs_[kRegTotalRows] <= 64;
}

DisplayController::Geometry DisplayController::geometry() const {
    Geometry g;
    const bool narrow = regs_[kRegMode0] & 0x10;
    g.cols = narrow ? 132 : 80;
    if (regs_[kRegMainRows] == 0) {
        // Timing block not yet loaded: the firmware already turns video on
        // during its RAM test. Assume a 24+1-row, 21-line default [hyp].
        g.rows = 24;
        g.status_rows = 1;
        g.cell_h = 21;
    } else {
        g.rows = regs_[kRegMainRows];
        g.status_rows = std::max(0, int(regs_[kRegTotalRows]) - g.rows);
        g.cell_h = (regs_[kRegCellLines] & 0x1F) + 1;
    }
    if (narrow) g.cell_w = 8;
    else if (regs_[kRegMode5] & 0x01) g.cell_w = 12;
    else g.cell_w = 16;
    g.main_start = kWindowBase + (uint32_t(regs_[kRegStart]) | uint32_t(regs_[kRegStart + 1]) << 8);
    g.status_start =
        kWindowBase + (uint32_t(regs_[kRegStatusStart]) | uint32_t(regs_[kRegStatusStart + 1]) << 8);
    return g;
}

bool DisplayController::cursor_on() const {
    return (regs_[layout_.cursor_on.reg] & layout_.cursor_on.mask) != 0;
}

std::string DisplayController::describe() const {
    const Geometry g = geometry();
    return format("  %s, %dx%d+%d cells of %dx%d px, main %05X, status %05X\n"
                  "  cursor row %u col %u (%s), frame %llu, retrace %d, irq %d, block pending %d\n",
                  !enabled() ? "blanked" : (regs_[kRegDim] & kDimData) ? "dimmed" : "enabled", g.cols, g.rows, g.status_rows, g.cell_w, g.cell_h,
                  g.main_start, g.status_start, regs_[kRegCursorRow], regs_[kRegCursorCol],
                  cursor_on() ? "on" : "off", (unsigned long long)frames_,
                  in_retrace_ ? 1 : 0, irq_pending_ ? 1 : 0, block_pending_ ? 1 : 0) +
           format("  video signature: shim %s (temporary), %s\n", vdo_shim_ ? "on" : "off",
                  sig_measuring_ ? "measuring"
                  : sig_valid_   ? format("result %X", sig_result_).c_str()
                                 : "idle");
}

uint8_t DisplayController::mem(uint32_t addr) const {
    if (ram_ && addr >= ram_->base && addr - ram_->base < ram_->storage.size())
        return ram_->storage[addr - ram_->base];
    return bus_.peek8(addr);
}

void DisplayController::draw_row(Framebuffer& fb, uint32_t addr, int y0, int cols, int cell_w,
                              int cell_h, int cursor_col, bool blink_phase) {
    const bool colour = colour_;
    const int ul_top = std::min<int>(regs_[kRegLineTop], cell_h - 1);
    const int ul_bottom = std::min<int>(std::max<int>(regs_[kRegLineBottom], ul_top), cell_h - 1);
    for (int c = 0; c < cols; ++c) {
        const uint32_t cell = addr + uint32_t(c) * 3;
        const uint8_t code = mem(cell);
        const CellLook l = decode_attr(mem(cell + 1), mem(cell + 2), colour, layout_.cells);
        const uint32_t glyph = kCharGenBase + (l.font2 ? 0x4000u : 0u) + uint32_t(code) * 32;
        const bool show = !l.hidden && !(l.blink && !blink_phase);
        const bool cursor = c == cursor_col;
        // Pixels per dot: two when dots are widened by half a dot.
        const int sub = layout_.half_dot_stretch ? 2 : 1;
        const int width = cell_w * sub;   // at most 32
        const uint32_t full = width >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> width);
        uint32_t* px = fb.row(y0) + c * width;
        for (int line = 0; line < cell_h; ++line, px += fb.width) {
            // The cell's pixels, leftmost in bit 31.
            uint32_t bits = 0;
            if (show && line < 32) {
                uint16_t dots = uint16_t(mem(glyph + uint32_t(line)) << 8);
                if (cell_w > 8) dots = uint16_t(dots | mem(glyph + 0x2000 + uint32_t(line)));
                if (sub == 1) {
                    bits = uint32_t(dots) << 16;
                } else {
                    for (int x = 0; x < cell_w; ++x)
                        if (dots & (0x8000u >> x)) bits |= 0xC0000000u >> (2 * x);
                    // Widened before underline and reverse, so that the dark
                    // strokes of reversed text do not close up [hypothesis].
                    bits |= bits >> 1;
                }
            }
            if (show && l.underline && line >= ul_top && line <= ul_bottom) bits = full;
            if (show && l.column_sep) bits |= sub == 1 ? 0x80000000u : 0xC0000000u;
            if (l.reverse) bits = ~bits;
            if (cursor) bits = ~bits;   // block cursor [hyp]
            for (int x = 0; x < width; ++x) px[x] = (bits & (0x80000000u >> x)) ? l.fg : kBlack;
        }
    }
}

void DisplayController::render(Framebuffer& fb) {
    // The 3477 uses a 4:3 CRT with a black border around the raster.
    fb.display_aspect = kDisplayAspect;
    fb.border_x = kDisplayBorderX;
    fb.border_y = kDisplayBorderY;
    if (!enabled()) {
        const int w = last_w_ ? last_w_ : 80 * 12, h = last_h_ ? last_h_ : 25 * 21;
        if (fb.width != w || fb.height != h) fb.resize(w, h);
        std::fill(fb.pixels.begin(), fb.pixels.end(), kBlack);
        return;
    }
    const Geometry g = geometry();
    const int w = g.cols * g.cell_w * (layout_.half_dot_stretch ? 2 : 1);
    const int h = (g.rows + g.status_rows) * g.cell_h;
    if (fb.width != w || fb.height != h) fb.resize(w, h);
    fb.pixels_per_dot = layout_.half_dot_stretch ? 2 : 1;
    fb.dot_pitch = layout_.beam_reference_dots ? double(layout_.beam_reference_dots) / double(w) : 1.0;
    last_w_ = w;
    last_h_ = h;

    ram_ = bus_.region_at(kWindowBase);
    if (ram_ && ram_->kind != Region::Kind::Ram) ram_ = nullptr;

    // Auto dim: after the setup's interval without a key the firmware sets
    // 20129 bit 7, and clears it at the next key [observed on the 3477,
    // 3477-FC and 3487; set at no other time]. A dimmed terminal shows only
    // its status line [doc 3489 UG 5.2.7], so the bit is taken to blank the
    // rows above it [hypothesis].
    const bool dimmed = (regs_[kRegDim] & kDimData) != 0;
    const bool blink_phase = (frames_ / kBlinkFrames) % 2 == 0;
    const int cur_row = cursor_on() ? int(regs_[kRegCursorRow]) : -1;
    const int cur_col = regs_[kRegCursorCol];
    const uint32_t stride = uint32_t(g.cols) * 3;
    for (int r = 0; r < g.rows + g.status_rows; ++r) {
        const bool status = r >= g.rows;
        if (dimmed && !status) {
            std::fill_n(fb.row(r * g.cell_h), size_t(w) * size_t(g.cell_h), kBlack);
            continue;
        }
        const uint32_t base = status ? g.status_start + uint32_t(r - g.rows) * stride
                                     : g.main_start + uint32_t(r) * stride;
        draw_row(fb, base, r * g.cell_h, g.cols, g.cell_w, g.cell_h, r == cur_row ? cur_col : -1,
                 blink_phase);
    }
    // Rule above the status line [hyp: 20128 bit 6]. On a real tube it is
    // drawn on the underline scan lines of the last main row, across the full
    // width (observed: photographs of a 3486, where it lines up exactly with
    // the field underlines and has their thickness).
    if (g.status_rows > 0 && g.rows > 0 && (regs_[kRegControl] & kCtlStatusRule)) {
        // Blue on a colour unit, as its status line is [hyp: the rule's
        // colour is not in any register seen].
        const uint32_t colour = colour_ ? kBlue : kGreyNormal;
        const int ul_top = std::min<int>(regs_[kRegLineTop], g.cell_h - 1);
        const int ul_bottom = std::min<int>(std::max<int>(regs_[kRegLineBottom], ul_top), g.cell_h - 1);
        for (int line = ul_top; line <= ul_bottom; ++line)
            std::fill_n(fb.row((g.rows - 1) * g.cell_h + line), w, colour);
    }
    ram_ = nullptr;
}

// Change tracking (DisplaySource::animation_phase): RAM and register writes
// are observed on the bus (model_3477.cpp); only the blink phase changes the
// picture on its own.
uint64_t DisplayController::animation_phase() const { return frames_ / kBlinkFrames; }

// ---------------------------------------------------------------------------
// TEMPORARY: video signature shim for the power-on display test (VDO).
//
// The hardware behind 2012F bits 7-4 folds one field of video into a 4-bit
// signature; the function is not known (docs/bringup-3477.md, "Display
// test"). The firmware compares it with expected values for six test
// screens and halts POST on a mismatch. Until the function is measured on a
// real unit, this shim returns the measured result for those screens.
//
// It is keyed only on hardware-visible state: the selector written to
// 2012F bits 6-5 and a fingerprint of the frame the controller displayed
// (vdo_fingerprint(): registers that define the raster, plus every displayed
// cell and the glyph scan lines it fetches). The display type enters through
// the timing registers, which differ per tube. Nothing here looks at the
// CPU, the firmware's code addresses or its stack, and the table holds no
// ROM bytes: only fingerprints computed by this emulator and the 4-bit
// results the hardware is expected to measure for them (the results the
// display test requires, i.e. what a working unit reads).
//
// Replace this with the real signature function (and delete the table) once
// single-dot signatures or the gate array documentation are available.
// Switch: the model's "vdo-shim" option (--no-vdo-shim, [emulator] vdo_shim).
// ---------------------------------------------------------------------------
namespace {

struct VdoShimEntry {
    uint8_t selector;
    uint64_t fingerprint;
    uint8_t nibble;
    const char* screen;   // what the frame shows, for the log
};

// Calibration table (temporary). Fingerprints of the display test's screens
// as computed by vdo_fingerprint() with this emulator; nibbles are the
// signatures a working unit measures for them. Display types 3 and 4 use
// identical raster settings, so their entries coincide. The first twelve
// entries are the 3477's.
constexpr VdoShimEntry kVdoShimTable[] = {
    {1, 0xCB6EF411973D33EAULL, 0x2, "24x80 corner cells (types 3/4)"},
    {1, 0xEFC13297CCF74DBCULL, 0x0, "nearly empty (types 3/4)"},
    {1, 0x1FCA31D7A1B20935ULL, 0x6, "nearly empty + cursor (types 3/4)"},
    {1, 0x409CB7A1EE91713EULL, 0xF, "27x132 corner cells (types 3/4)"},
    {1, 0x5AD4EEB6A5C7463BULL, 0x9, "43x80 corner cells (types 3/4)"},
    {2, 0xCB6EF411973D33EAULL, 0x2, "24x80 corner cells (types 3/4)"},
    {1, 0xECA33D0B60D0E183ULL, 0xA, "24x80 corner cells (type 5)"},
    {2, 0xECA33D0B60D0E183ULL, 0x5, "24x80 corner cells (type 5)"},
    {1, 0x8BB1434A1854F05DULL, 0xF, "nearly empty (type 5)"},
    {1, 0x6DBCB42E33A89264ULL, 0x1, "nearly empty + cursor (type 5)"},
    {1, 0x16BE48B1B1006520ULL, 0xB, "27x132 corner cells (type 5)"},
    {1, 0xCEADB1520B23E369ULL, 0x3, "43x80 corner cells (type 5)"},
    // 3486 family (firmware 54G0010), by strap code; the selector is 2012F
    // bits 5-4 there. The screens are named by the test's own step number
    // until they are described.
    {1, 0xF867E26C6D595335ULL, 0xD, "3486 type 2, test step 11"},
    {2, 0xF867E26C6D595335ULL, 0x2, "3486 type 2, test step 12"},
    {1, 0xBB04DAA9FE1C1D5FULL, 0x0, "3486 type 2, test step 15"},
    {1, 0x724F3A9CC8C05CF6ULL, 0x1, "3486 type 2, test step 14"},
    {1, 0x06D29123965E2579ULL, 0x7, "3487 types 3/4, test step 31"},
    {2, 0x06D29123965E2579ULL, 0x2, "3487 types 3/4, test step 32"},
    {1, 0xD606F3E4E2414E43ULL, 0x0, "3487 types 3/4, test step 35"},
    {1, 0xA34459C71745BE86ULL, 0xB, "3487 types 3/4, test step 34"},
    {1, 0x7EB2FC1F8B8C0653ULL, 0x0, "3487 types 3/4, test step 38"},
    {1, 0x40BC8EDA74B4A35AULL, 0xC, "3487 types 3/4, test step 37"},
    {1, 0x7E3F79BCBBEBCCA9ULL, 0x1, "3487 types 3/4, test step 36"},
    {1, 0xFE7611BE9AD25149ULL, 0xA, "3487 types 3/4, test step 39"},
    {1, 0xB86B40035AF58E66ULL, 0x9, "3487 type 5, test step 51"},
    {2, 0xB86B40035AF58E66ULL, 0xC, "3487 type 5, test step 52"},
    {1, 0x337B49F6FC32910CULL, 0xE, "3487 type 5, test step 55"},
    {1, 0xF7A5C432B57F9FF9ULL, 0x7, "3487 type 5, test step 54"},
    {1, 0xC898BF6D03E2D95DULL, 0x3, "3487 type 5, test step 58"},
    {1, 0x52F93C4F962DE12DULL, 0x3, "3487 type 5, test step 57"},
    {1, 0x921C28E89DB78EC6ULL, 0x3, "3487 type 5, test step 56"},
    {1, 0xBF47EAAA2EA0983AULL, 0x9, "3487 type 5, test step 59"},
    {1, 0xA2120A9B118403F6ULL, 0x0, "3487 type 6, test step 61"},
    {2, 0xA2120A9B118403F6ULL, 0x2, "3487 type 6, test step 62"},
    {3, 0xA2120A9B118403F6ULL, 0x2, "3487 type 6, test step 63"},
    {1, 0xD4050AD46AA479FCULL, 0x0, "3487 type 6, test step 65"},
    {3, 0x69F130DBEB3E3465ULL, 0x6, "3487 type 6, test step 64"},
    {1, 0xF93F87A0C99CE579ULL, 0xD, "3487 type 6, test step 68"},
    {1, 0x1DA06DE3C68A2052ULL, 0x7, "3487 type 6, test step 67"},
    {1, 0x82D25E4F809D9DEEULL, 0xA, "3487 type 6, test step 66"},
    // The colour 3477 (type 6): the values its display test accepts, in
    // the order it measures. They equal the 3487 colour unit's for the
    // corresponding screens.
    {1, 0x435A124BA0C88369ULL, 0x0, "3477 type 6, 24x80 corner cells"},
    {2, 0x435A124BA0C88369ULL, 0x2, "3477 type 6, 24x80 corner cells"},
    {3, 0x435A124BA0C88369ULL, 0x2, "3477 type 6, 24x80 corner cells"},
    {1, 0x4FD934339AFE316BULL, 0x0, "3477 type 6, second screen"},
    {3, 0x73EAD52D49223EAEULL, 0x6, "3477 type 6, third screen"},
    {1, 0x274AD2C3BE0853E9ULL, 0xD, "3477 type 6, fourth screen"},
    {1, 0x86CF80956AF7D2FBULL, 0x7, "3477 type 6, fifth screen"},
};

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001B3ull;

inline void fnv(uint64_t& h, uint8_t b) {
    h ^= b;
    h *= kFnvPrime;
}

} // namespace

uint64_t DisplayController::vdo_fingerprint() const {
    uint64_t h = kFnvOffset;
    // Raster definition: CRT timing block and cursor position (00-18), mode
    // block (25-2E: cell width, column mode, cursor, video control and
    // inversion), window start addresses (30-35).
    for (uint32_t r = 0x00; r <= 0x18; ++r) fnv(h, regs_[r]);
    for (uint32_t r = 0x25; r <= 0x2E; ++r) fnv(h, regs_[r]);
    for (uint32_t r = 0x30; r <= 0x35; ++r) fnv(h, regs_[r]);
    if (!enabled()) return h;
    // What the controller fetches for every displayed cell.
    const Geometry g = geometry();
    const uint32_t stride = uint32_t(g.cols) * 3;
    for (int row = 0; row < g.rows + g.status_rows; ++row) {
        const bool status = row >= g.rows;
        const uint32_t base = status ? g.status_start + uint32_t(row - g.rows) * stride
                                     : g.main_start + uint32_t(row) * stride;
        for (int c = 0; c < g.cols; ++c) {
            const uint32_t cell = base + uint32_t(c) * 3;
            const uint8_t code = bus_.peek8(cell);
            const uint8_t attr = bus_.peek8(cell + 1);
            fnv(h, code);
            fnv(h, attr);
            fnv(h, bus_.peek8(cell + 2));
            const uint32_t glyph = kCharGenBase + ((attr & 0x80) ? 0x4000u : 0u) + uint32_t(code) * 32;
            for (int line = 0; line < g.cell_h && line < 32; ++line) {
                fnv(h, bus_.peek8(glyph + uint32_t(line)));
                if (g.cell_w > 8) fnv(h, bus_.peek8(glyph + 0x2000 + uint32_t(line)));
            }
        }
    }
    return h;
}

uint8_t DisplayController::vdo_shim_signature(int selector) const {
    selector &= 3;
    const uint64_t fp = vdo_fingerprint();
    for (const VdoShimEntry& e : kVdoShimTable) {
        if (e.selector == selector && e.fingerprint == fp) {
            LOG_DEBUG("display", "vdo shim: selector %d, frame %016llX (%s) -> %X", selector,
                      (unsigned long long)fp, e.screen, e.nibble);
            return e.nibble;
        }
    }
    // Unknown frame: a placeholder that depends on the frame and selector.
    // Selector 00 is only required to be non-zero [obs], so it never reads 0.
    uint8_t v = 0;
    for (int i = 0; i < 64; i += 4) v ^= uint8_t((fp >> i) & 0x0F);
    v = uint8_t((v ^ selector) & 0x0F);
    if (selector == 0 && v == 0) v = 1;
    LOG_DEBUG("display", "vdo shim: selector %d, frame %016llX (not in table) -> %X", selector,
              (unsigned long long)fp, v);
    return v;
}

} // namespace em34xx
