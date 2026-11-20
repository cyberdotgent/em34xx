#include "devices/ibm3477/display.hpp"

#include "devices/i2c.hpp"
#include "util/strings.hpp"

#include <algorithm>

namespace tn34xx {

namespace {
constexpr double kDisplayAspect = 4.0 / 3.0;
// Black margin around the raster on each side, as a fraction of the tube.
// Hypothesis, matched to a reference recording (~2.5%); the exact value
// should eventually come from the CRT timing registers.
constexpr double kDisplayBorder = 0.025;
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
constexpr uint32_t kRegMode5 = 0x2A;       // bit0: 12-pixel cells, else 16
constexpr uint32_t kRegStatus = 0x2F;
constexpr uint32_t kRegStart = 0x30;
constexpr uint32_t kRegStatusStart = 0x34;
constexpr uint32_t kRegIrqAck = 0x36;
constexpr uint32_t kRegBlockStart = 0x38;  // block request; written 3B, 3A, 39, 38

constexpr uint8_t kCtlVideo = 0x10;
constexpr uint8_t kCtlStatusRule = 0x40;
constexpr uint8_t kCtlCursor = 0x80;

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
constexpr uint32_t kGreyNormal = 0xFFB0B0B0u;
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

// Attribute byte. Observed values: 20 for cleared screen cells, 21 for the
// one reverse cell of the power-on display test, A0 for a character that
// the firmware takes from the second font, 00 and 40 in the status line.
// Bit 7 selects font 2 [obs]. Bits 4..0 are taken to be the 5250 screen
// attribute (20h + bits) [hyp]: bit0 reverse, bit1 high intensity, bit2
// underline, bit3 blink, bit4 column separators, with 7 in bits 2..0 meaning
// non-display. On colour units the same bits select the 5250 colours.
// Bits 6 and 5 and the extension byte are not interpreted.
CellLook decode_attr(uint8_t attr, bool colour) {
    CellLook l;
    l.font2 = attr & 0x80;
    const uint8_t a = attr & 0x1F;
    if ((a & 0x07) == 0x07) {
        l.hidden = true;
        return l;
    }
    l.reverse = a & 0x01;
    l.underline = a & 0x04;
    const bool hi = a & 0x02;
    if (!colour) {
        l.fg = hi ? kGreyBright : kGreyNormal;
        l.blink = a & 0x08;
        l.column_sep = a & 0x10;
        return l;
    }
    switch (a >> 3) {
    case 0: l.fg = hi ? kWhite : kGreen; break;
    case 1: l.fg = kRed; l.blink = hi; break;
    case 2: l.fg = hi ? kYellow : kTurquoise; l.column_sep = !l.underline; break;
    default: l.fg = hi ? kBlue : kPink; break;
    }
    return l;
}

} // namespace

Ibm3477Display::Ibm3477Display(Bus& bus, const TerminalOptions& opts)
    : Device("display"), bus_(bus), opts_(opts) {
    reset();
}

void Ibm3477Display::set_irq(std::function<void(bool)> line) { irq_ = std::move(line); }
void Ibm3477Display::set_i2c(I2cBus* bus) { i2c_ = bus; }

void Ibm3477Display::set_clock_hz(uint64_t hz) {
    clock_hz_ = hz ? hz : 8000000;
    recompute_timing();
}

void Ibm3477Display::reset() {
    regs_.fill(0);
    phase_ = 0;
    frames_ = 0;
    in_retrace_ = false;
    block_pending_ = false;
    set_irq_line(false);
    recompute_timing();
}

void Ibm3477Display::recompute_timing() {
    frame_cycles_ = std::max<uint64_t>(clock_hz_ / kRefreshHz, 100);
    // Vertical sync of a few scan lines out of a ~525-line frame [hyp].
    retrace_cycles_ = std::max<uint64_t>(frame_cycles_ / 90, 1);
    phase_ %= frame_cycles_;
}

void Ibm3477Display::set_irq_line(bool on) {
    if (irq_pending_ == on) return;
    irq_pending_ = on;
    if (irq_) irq_(on);
}

void Ibm3477Display::tick(uint64_t cycles) {
    phase_ += cycles;
    bool frame_started = false;
    while (phase_ >= frame_cycles_) {
        phase_ -= frame_cycles_;
        ++frames_;
        frame_started = true;
    }
    // Each frame begins with vertical retrace.
    const bool retrace = phase_ < retrace_cycles_;
    if ((frame_started || (retrace && !in_retrace_)) && block_pending_) {
        // A block request completes at the next vertical retrace [hyp].
        block_pending_ = false;
        set_irq_line(true);
    }
    in_retrace_ = retrace;
}

uint8_t Ibm3477Display::read_status() const {
    const uint8_t w = regs_[kRegStatus];
    uint8_t v = uint8_t(sense_ << 4);
    if ((w & 0x01) || (i2c_ && !i2c_->sda_in())) v |= 0x01;
    if (w & 0x02) v |= 0x02;
    if (in_retrace_) v |= 0x08;
    return v;
}

uint8_t Ibm3477Display::mmio_read8(uint32_t offset) {
    offset &= 0xFF;
    if (offset == kRegStatus) return read_status();
    return regs_[offset];
}

uint8_t Ibm3477Display::peek8(uint32_t offset) { return mmio_read8(offset); }

void Ibm3477Display::mmio_write8(uint32_t offset, uint8_t value) {
    offset &= 0xFF;
    regs_[offset] = value;
    switch (offset) {
    case kRegStatus:
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

bool Ibm3477Display::enabled() const {
    // 20128 is 00 while the firmware reloads the character generator [obs];
    // bit 4 is set whenever a screen is meant to be visible [hyp].
    return (regs_[kRegControl] & kCtlVideo) && regs_[kRegMainRows] <= 64 &&
           regs_[kRegTotalRows] <= 64;
}

Ibm3477Display::Geometry Ibm3477Display::geometry() const {
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

std::string Ibm3477Display::describe() const {
    const Geometry g = geometry();
    return format("  %s, %dx%d+%d cells of %dx%d px, main %05X, status %05X\n"
                  "  cursor row %u col %u (%s), frame %llu, retrace %d, irq %d, block pending %d\n",
                  enabled() ? "enabled" : "blanked", g.cols, g.rows, g.status_rows, g.cell_w, g.cell_h,
                  g.main_start, g.status_start, regs_[kRegCursorRow], regs_[kRegCursorCol],
                  (regs_[kRegControl] & kCtlCursor) ? "on" : "off", (unsigned long long)frames_,
                  in_retrace_ ? 1 : 0, irq_pending_ ? 1 : 0, block_pending_ ? 1 : 0);
}

uint8_t Ibm3477Display::mem(uint32_t addr) const {
    if (ram_ && addr >= ram_->base && addr - ram_->base < ram_->storage.size())
        return ram_->storage[addr - ram_->base];
    return bus_.peek8(addr);
}

void Ibm3477Display::draw_row(Framebuffer& fb, uint32_t addr, int y0, int cols, int cell_w,
                              int cell_h, int cursor_col, bool blink_phase) {
    const bool colour = opts_.display == DisplaySubtype::Color;
    const int ul_top = std::min<int>(regs_[kRegLineTop], cell_h - 1);
    const int ul_bottom = std::min<int>(std::max<int>(regs_[kRegLineBottom], ul_top), cell_h - 1);
    for (int c = 0; c < cols; ++c) {
        const uint32_t cell = addr + uint32_t(c) * 3;
        const uint8_t code = mem(cell);
        const CellLook l = decode_attr(mem(cell + 1), colour);
        const uint32_t glyph = kCharGenBase + (l.font2 ? 0x4000u : 0u) + uint32_t(code) * 32;
        const bool show = !l.hidden && !(l.blink && !blink_phase);
        const bool cursor = c == cursor_col;
        uint32_t* px = fb.row(y0) + c * cell_w;
        for (int line = 0; line < cell_h; ++line, px += fb.width) {
            uint16_t bits = 0;
            if (show && line < 32) {
                bits = uint16_t(mem(glyph + uint32_t(line)) << 8);
                if (cell_w > 8) bits = uint16_t(bits | mem(glyph + 0x2000 + uint32_t(line)));
            }
            if (show && l.underline && line >= ul_top && line <= ul_bottom) bits = 0xFFFF;
            if (show && l.column_sep) bits |= 0x8000;
            if (l.reverse) bits = uint16_t(~bits);
            if (cursor) bits = uint16_t(~bits);   // block cursor [hyp]
            for (int x = 0; x < cell_w; ++x) px[x] = (bits & (0x8000u >> x)) ? l.fg : kBlack;
        }
    }
}

void Ibm3477Display::render(Framebuffer& fb) {
    // The 3477 uses a 4:3 CRT with a black border around the raster.
    fb.display_aspect = kDisplayAspect;
    fb.border = kDisplayBorder;
    if (!enabled()) {
        const int w = last_w_ ? last_w_ : 80 * 12, h = last_h_ ? last_h_ : 25 * 21;
        if (fb.width != w || fb.height != h) fb.resize(w, h);
        std::fill(fb.pixels.begin(), fb.pixels.end(), kBlack);
        return;
    }
    const Geometry g = geometry();
    const int w = g.cols * g.cell_w;
    const int h = (g.rows + g.status_rows) * g.cell_h;
    if (fb.width != w || fb.height != h) fb.resize(w, h);
    last_w_ = w;
    last_h_ = h;

    ram_ = bus_.region_at(kWindowBase);
    if (ram_ && ram_->kind != Region::Kind::Ram) ram_ = nullptr;

    const bool blink_phase = (frames_ / kBlinkFrames) % 2 == 0;
    const int cur_row = (regs_[kRegControl] & kCtlCursor) ? int(regs_[kRegCursorRow]) : -1;
    const int cur_col = regs_[kRegCursorCol];
    const uint32_t stride = uint32_t(g.cols) * 3;
    for (int r = 0; r < g.rows + g.status_rows; ++r) {
        const bool status = r >= g.rows;
        const uint32_t base = status ? g.status_start + uint32_t(r - g.rows) * stride
                                     : g.main_start + uint32_t(r) * stride;
        draw_row(fb, base, r * g.cell_h, g.cols, g.cell_w, g.cell_h, r == cur_row ? cur_col : -1,
                 blink_phase);
    }
    // Rule above the status line [hyp: 20128 bit 6].
    if (g.status_rows > 0 && (regs_[kRegControl] & kCtlStatusRule)) {
        const uint32_t colour =
            opts_.display == DisplaySubtype::Color ? kGreen : kGreyNormal;
        std::fill_n(fb.row(g.rows * g.cell_h), w, colour);
    }
    ram_ = nullptr;
}

} // namespace tn34xx
