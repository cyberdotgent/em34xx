// SPDX-License-Identifier: GPL-3.0-or-later
// See board_3483.hpp. Register roles are taken from the firmware's use
// (observed in code) unless marked hypothesis.
#include "hw/board_3483.hpp"

#include "bus/bus.hpp"
#include "hw/i80186_peripherals.hpp"
#include "hw/terminal_colours.hpp"
#include "input/set3_keyboard.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>

namespace em34xx {

// --- board -------------------------------------------------------------------

Board3483::Board3483(Bus& bus, I80186Peripherals* cpu_io, uint64_t clock_hz, uint8_t monitor_sense, Layout layout)
    : bus_(bus), cpu_io_(cpu_io), layout_(layout), display_(nullptr), coax_(nullptr), system_(nullptr),
      monitor_sense_(monitor_sense) {
    // 60 Hz frame with a short vertical sync [hypothesis]. POST measures
    // both phases in loop counts and accepts a window around this.
    frame_cycles_ = clock_hz / 60;
    active_cycles_ = frame_cycles_ - frame_cycles_ / 40;
    if (cpu_io_) cpu_io_->set_cascade_controller(0, this);
}

void Board3483::advance(uint64_t cycles) {
    frame_pos_ += cycles;
    while (frame_pos_ >= frame_cycles_) {
        frame_pos_ -= frame_cycles_;
        ++frame_count_;
        // A frame with the video test on (5801A bit 5) and no output
        // selected (58021 bits 0-2 = 0) raises 5810C bit 5 [hypothesis
        // fitted to the display test: it expects the interrupt for that
        // case only].
        if (display_ && (display_->reg(0x1A) & 0x20) && ((display_->reg(0x20) >> 8) & 7) == 0) raise1(0x0020);
    }
}

void Board3483::raise1(uint16_t bits) {
    status1_ |= bits;
    update_irq();
}

void Board3483::raise2(uint16_t bits) {
    status2_ |= bits;
    update_irq();
}

// INT0 is the OR of the enabled status bits: 5810E enables 5810C bits and
// 58122 enables 58120 bits [hypothesis: both are written 0 at reset and
// set later, and POST polls the status registers with them 0].
bool Board3483::intr_asserted() const {
    const uint16_t en1 = coax_ ? coax_->reg(0x0E) : 0, en2 = coax_ ? coax_->reg(0x22) : 0;
    return (status1_ & en1) != 0 || (status2_ & en2) != 0;
}

void Board3483::update_irq() {
    // Without the 80186 the CPU asks intr_asserted() itself.
    if (cpu_io_) cpu_io_->set_int_line(0, intr_asserted());
}

void Board3483::set_window(bool on) {
    if (on == window_mapped_ || !window_) return;
    if (on) bus_.map_mmio("CG write window", layout_.sram, 0x8000, window_);
    else bus_.unmap_device_mmio(window_);
    window_mapped_ = on;
}

// --- 40000 write window --------------------------------------------------------

uint8_t* Board3483::Window::sram(uint32_t offset) {
    // The SRAM region itself, beneath the window (its upper half is never
    // covered).
    const uint32_t base = board_.layout_.sram;
    const Region* r = board_.bus_.region_at(base + 0x8000);
    if (!r || r->kind != Region::Kind::Ram) return nullptr;
    const uint32_t a = base + offset;
    if (a < r->base || a >= r->base + r->size) return nullptr;
    return const_cast<uint8_t*>(&r->storage[a - r->base]);
}

uint8_t Board3483::Window::mmio_read8(uint32_t offset) {
    const uint8_t* p = sram(offset);
    return p ? *p : 0xFF;
}

// One word per glyph row: the word at window offset w is row w/2 of the
// character generator, its low byte in the first plane (40000 + w/2) and
// its high byte in the second (48000 + w/2). The 32 KiB window so spans
// 16 KiB of each plane, the base and APL/GE sets with their companions.
void Board3483::Window::mmio_write16(uint32_t offset, uint16_t value) {
    const uint32_t row = (offset & ~1u) >> 1;
    if (uint8_t* lo = sram(row)) *lo = uint8_t(value);
    if (uint8_t* hi = sram(row + 0x8000)) *hi = uint8_t(value >> 8);
}

void Board3483::Window::mmio_write8(uint32_t offset, uint8_t value) {
    // A byte write: the even byte is the low (first-plane) half [hypothesis].
    const uint32_t row = offset >> 1;
    if (uint8_t* p = sram((offset & 1) ? row + 0x8000 : row)) *p = value;
}

// --- display controller (58000) ----------------------------------------------

namespace {

// Character sets as the firmware decompresses them into SRAM, 32 bytes
// per character [observed]: the base 3270 set at 48000 and the APL/GE set
// at 4A000, each with a sparse companion plane 4000h below (40000, 42000)
// that holds the dots right of the first eight [hypothesis: the window's
// word layout puts the low byte at 40000 and the high byte at 48000].
constexpr uint32_t kCgSets[2] = {0x8000, 0xA000};   // from the SRAM base
constexpr uint32_t kCgCompanion = 0x8000;

// The video sense's threshold on a gun's level from the drive table at
// 58040-6F [observed: POST test 7's own table puts its levels just below
// (B4, C0) and just above (D8, DC, E0) one point, and expects colour 4,
// levels 70/C0/DC, to light only the third gun; the exact point between C0
// and D8 is a hypothesis].
constexpr uint8_t kSenseThreshold = 0xCC;

// Start of the regen buffer: cell 800h, the bias of row-list addresses
// [hypothesis, see row_address()].
constexpr uint32_t kRegenOrigin = 0x1000;

// The status row (3270 operator information area): 80 cells at 0:0800,
// shown below the screen's rows with a rule along its top edge [observed:
// the runtime keeps "SETUP" in reverse white there, and photos of a real
// 3483 show that bar under a thin blue line on every setup screen;
// hypothesis: that the address is fixed, no register seen points to it].
constexpr uint32_t kStatusRow = 0x0800;

// Frames per blink phase (cursor and blinking characters): about 1 Hz at
// 60 Hz [hypothesis].
constexpr uint64_t kBlinkFrames = 32;

} // namespace

Board3483::Display::Display(Board3483& board) : Device("display-3483"), board_(board) { board_.display_ = this; }

void Board3483::Display::reset() {
    regs_.fill(0);
    board_.set_window(false);
}

uint16_t Board3483::Display::read_word(unsigned off) const {
    off &= 0xFE;
    if (off == 0x20) {
        // 58020 (read): video drive sense [hypothesis]: the number of the
        // three outputs selected by 58021 bits 0-2 that carried a lit dot
        // while video was on (58110 bit 0) [observed: POST test 7 expects
        // 3->2, 2->1, 4->1 over its first screens, 3->0 over a screen of
        // colour-4 cells, and 0 over a blank screen or with video off]. A gun counts when a displayed dot drove
        // it above the sense threshold (lit_guns()).
        const uint16_t sel = uint16_t((regs_[0x20 >> 1] >> 8) & 7);
        const bool video = board_.coax_ && (board_.coax_->reg(0x10) & 1);
        unsigned n = 0;
        if (video)
            for (uint16_t b = uint16_t(sel & lit_guns()); b; b &= uint16_t(b - 1)) ++n;
        return uint16_t((regs_[0x20 >> 1] & 0xFF00) | n);
    }
    if (off == 0x26) {
        // Bit 3: the internal service table (no option ROM). Bit 0: sync
        // polarity of the monitor, 1 = vertical sync reads as 1 in 5840E
        // bit 2 [observed use; value hypothesis].
        return uint16_t((regs_[off >> 1] & ~0x0009) | 0x0009);
    }
    return regs_[off >> 1];
}

void Board3483::Display::write_word(unsigned off, uint16_t value) {
    off &= 0xFE;
    regs_[off >> 1] = value;
    if (off == 0x20) board_.set_window((value & 0x8000) != 0);   // 58021 bit 7
}

// The display geometry comes from the mode record the firmware loads at
// D000:564C (12 records; 24x80, 32x80, 43x80 and 27x132 among them)
// [observed: record contents and the registers they go to]:
//  - 58000 low byte = columns - 1 (4F, or 83 for 132 columns);
//  - 58006 low byte = rows - 1 (17, 1F, 2A, 1A);
//  - 5800A low byte = scan lines per cell (12, 0D, 09, 10) [observed: POST's
//    blank test screen holds glyph 02, whose only dots are in its bytes
//    18-19, and expects no light with 12h here, so a cell shows bytes
//    0-17 of its glyph].
// Scan lines are numbered from 1 in the cursor register 58016, so the
// records' 1211, 0D0C, 0908 are the last two lines of 18-, 13- and 9-line
// cells (and 0909 the last line) [hypothesis, the only reading under which
// every record puts the cursor inside the cell].
unsigned Board3483::Display::rows() const {
    const unsigned n = (regs_[0x06 >> 1] & 0xFF) + 1;
    return n >= 2 && n <= 62 ? n : 24;
}

unsigned Board3483::Display::columns() const {
    const unsigned n = (regs_[0] & 0xFF) + 1;
    return n >= 40 && n <= 160 ? n : 80;
}

unsigned Board3483::Display::cell_height() const {
    const unsigned h = regs_[0x0A >> 1] & 0xFF;
    return h >= 8 && h <= 32 ? h : 18;
}

uint32_t Board3483::Display::row_address(unsigned row) const {
    // 5801A bit 6 set: linear, rows follow each other from the regen origin
    // 0:1000 [observed: POST's own screens set it and write their text from
    // 0:1000]. 5801C is not a start address: the runtime keeps control bits
    // in it (cursor on/shape/blink, see cursor()) [observed].
    //
    // Clear: the row list in block RAM [observed: svc_244 copies it there],
    // entry e at A = 50108+4e and B = 50508+4e, cell ((A[0] & 7F) << 8 |
    // B[0]) stored with a bias of 800h cells, so the byte address is twice
    // that [observed: the firmware adds 800h itself, C000:CBCD, D000:5189].
    // The controller walks the list downward from entry 61, one entry per
    // character row, and the entry it reads during a row sets where the next
    // row starts: so row r >= 1 comes from entry 62 - r, and row 0 from the
    // entries read during vertical blanking [observed: the runtime's builder
    // (D000:4F50, D000:513C) writes 62 entries (0-61), row r's start at entry
    // 62 - r for r >= 1, and the regen origin (0800) a little below the last
    // row, in 24, 32 and 43 rows]. An entry with A bit 7 clear gives its
    // cell; bit 7 set continues after the previous row. Row 0 starts at the
    // cell of the lowest entry below the screen with bit 7 clear, the last
    // one read before the frame [hypothesis: fits that runtime layout and
    // all six of POST test 7's video sense readings, over lists of one
    // repeated entry]; with none, at the regen origin.
    const unsigned cols = columns();
    if (regs_[0x1A >> 1] & 0x0040) return kRegenOrigin + 2u * row * cols;
    Bus& bus = board_.bus_;
    const uint32_t list_a = board_.layout_.block_ram + 0x108, list_b = board_.layout_.block_ram + 0x508;
    auto starts_row = [&](unsigned e) { return !(bus.peek8(list_a + 4 * e) & 0x80); };
    auto entry_cell = [&](unsigned e) {
        return uint32_t(bus.peek8(list_a + 4 * e) & 0x7F) << 8 | bus.peek8(list_b + 4 * e);
    };
    constexpr unsigned kLast = 61;   // the list is entries 0-61
    const unsigned shown = std::min(rows(), kLast + 1);
    uint32_t addr = kRegenOrigin;
    for (unsigned e = kLast + 1 - shown; e-- > 0;)
        if (starts_row(e)) addr = 2 * entry_cell(e);
    for (unsigned r = 0;; ++r) {
        uint32_t start = addr;
        if (r > 0) {
            const unsigned e = kLast + 1 - std::min(r, kLast + 1);
            if (starts_row(e)) start = 2 * entry_cell(e);
        }
        if (r >= row) return start;
        addr = start + 2u * cols;
    }
}

// One cell as the display shows it [hypothesis throughout, fitted to the
// firmware's screens and POST's video sense]: a two-byte cell is a
// character in 3270 buffer code and a 3270 extended attribute (EAB) byte.
//  - A character C0-FF is a field attribute: it shows as background and
//    sets the cells after it: bits 3-2 = 11 non-display, 10 high
//    intensity, bit 5 protected [observed: FC hides Offline Setup's hidden
//    values, F8 is its title]. Its EAB byte is the field's extended
//    attribute.
//  - EAB bits 7-6: 00 none, 01 blink, 10 reverse, 11 underscore; bits 5-3
//    colour in 3270 numbering (1 blue, 2 red, 3 pink, 4 green, 5 turquoise,
//    6 yellow, 7 white); bits 2-0 character set, 1 = APL/GE [observed:
//    B9/09 is Offline Setup's rule, B9 a line in the GE set; C9/B8 marks the
//    selected item; the colour table the firmware loads has those colours
//    at those indices].
//  - A cell without a colour, highlight or character set takes its field's
//    [observed: Customization's underscore-style samples are B9 cells with
//    no EAB of their own in a field whose EAB selects the GE set; they are
//    lines, not the base set's "Z", on a real 3483]. A field without a
//    colour takes the 3279 base colours: blue (protected), red (high),
//    green, white (protected and high).
//  - The colour index selects a level per gun from the table at 58040-6F
//    [observed: table contents], which POST's video sense measures; the
//    picture draws the index with the shared terminal colours. Index 0 is
//    the background.
struct Board3483::Display::FieldState {
    bool nondisplay = false;
    bool high = false;
    bool protect = false;
    uint8_t eab = 0;
};

uint8_t Board3483::Display::level(unsigned gun, unsigned index) const {
    const unsigned off = 0x40 + 16 * gun + (index & 15);
    return uint8_t(regs_[off >> 1] >> ((off & 1) * 8));
}

// The cell's colour (1-7) and intensity, without drawing it; 0 for a field
// attribute or a non-display field.
unsigned Board3483::Display::cell_colour(uint8_t ch, uint8_t eab, const FieldState& field) const {
    if ((ch & 0xC0) == 0xC0 || field.nondisplay) return 0;
    unsigned colour = (eab >> 3) & 7;
    if (!colour) colour = (field.eab >> 3) & 7;
    if (!colour) colour = field.protect ? (field.high ? 7 : 1) : (field.high ? 2 : 4);
    return colour;
}

// Dots of scan line y of a cell (bit 15 = leftmost) and the colour index of
// its lit dots; unlit dots are index 0. `blink_off`: the blink phase in
// which blinking characters are dark.
uint16_t Board3483::Display::cell_dots(uint8_t ch, uint8_t eab, FieldState& field, unsigned y, unsigned cell_h,
                                       unsigned& index, bool blink_off) const {
    index = 0;
    if ((ch & 0xC0) == 0xC0) {
        field.nondisplay = (ch & 0x0C) == 0x0C;
        field.high = (ch & 0x0C) == 0x08;
        field.protect = (ch & 0x20) != 0;
        field.eab = eab;
        return 0;
    }
    if (field.nondisplay) return 0;
    index = cell_colour(ch, eab, field);
    const unsigned cs = (eab & 7) ? (eab & 7) : (field.eab & 7);
    const unsigned set = cs == 1 ? 1 : 0;
    const uint32_t at = board_.layout_.sram + kCgSets[set] + uint32_t(ch) * 32 + y;
    // The 9 dots of a cell are bits 6-0 of the main byte and bits 7-6 of
    // the companion [observed: the GE line glyph B9 lights exactly those,
    // and a rule of them is solid on a real 3483; text glyphs keep main bit
    // 7 clear].
    uint16_t bits = uint16_t((board_.bus_.peek8(at) << 8 | board_.bus_.peek8(at - kCgCompanion)) << 1);
    const uint8_t hl = (eab & 0xC0) ? (eab >> 6) : (field.eab >> 6);
    // Blink: the character goes dark in one phase [hypothesis for the rate:
    // the same phase as the cursor's blink].
    if (hl == 1 && blink_off) bits = 0;
    // Underscore on the underline cursor's lines [hypothesis: a photo of a
    // real 3483 puts the underline at 0.875-0.97 of the cell, lines 16-17 of
    // 18]; 58016 bit 7 clear makes it dashed [observed: Customization's
    // "Underscore style" toggles that bit alone; the dash pattern is a
    // hypothesis].
    if (hl == 3 && on_underline(y, cell_h)) bits |= underscore_dots();
    if (hl == 2) bits = uint16_t(~bits & 0xFF80);              // reverse, over the 9-dot cell
    return bits;
}

bool Board3483::Display::on_underline(unsigned y, unsigned cell_h) const {
    const Cursor c = cursor();
    const unsigned first = std::min(c.first_line, cell_h - 1), last = std::min(c.last_line, cell_h - 1);
    return y >= first && y <= last;
}

uint16_t Board3483::Display::underscore_dots() const {
    // Solid across the cell, or dashed: a short gap at each end so that
    // neighbouring cells give "- - -".
    return (regs_[0x16 >> 1] & 0x0080) ? uint16_t(0xFF80) : uint16_t(0x3E00);
}

// The field attribute in force at the start of the screen: the last one on
// it (3270 fields wrap from the end of the buffer to its start).
Board3483::Display::FieldState Board3483::Display::initial_field() const {
    FieldState f;
    unsigned idx;
    for (unsigned r = 0; r < rows(); ++r) {
        const uint32_t start = row_address(r);
        for (unsigned c = 0; c < columns(); ++c) {
            const uint8_t ch = board_.bus_.peek8(start + 2 * c);
            if ((ch & 0xC0) == 0xC0) cell_dots(ch, board_.bus_.peek8(start + 2 * c + 1), f, 0, 1, idx, false);
        }
    }
    return f;
}

// The guns that carried a dot above the sense threshold, as a bit mask.
unsigned Board3483::Display::lit_guns() const {
    const unsigned cell_h = cell_height();
    unsigned guns = 0;
    FieldState f = initial_field();
    for (unsigned r = 0; r < rows(); ++r) {
        const uint32_t start = row_address(r);
        for (unsigned c = 0; c < columns(); ++c) {
            const uint8_t ch = board_.bus_.peek8(start + 2 * c), eab = board_.bus_.peek8(start + 2 * c + 1);
            const FieldState before = f;
            for (unsigned y = 0; y < cell_h; ++y) {
                f = before;
                unsigned idx;
                if (!(cell_dots(ch, eab, f, y, cell_h, idx, false) & 0xFF80)) continue;
                for (unsigned g = 0; g < 3; ++g)
                    if (level(g, idx) > kSenseThreshold) guns |= 1u << g;
            }
            if (guns == 7) return guns;
        }
    }
    return guns;
}

Board3483::Display::Cursor Board3483::Display::cursor() const {
    // 58010: cursor row (high byte) and column (low byte) on the screen
    // [observed: the cursor routine D000:2F40 divides the buffer offset by
    // the line length and writes row:column; the display test puts it at
    // 0D:09]. 5801C bit 7 shows it [observed: set whenever the cursor is
    // placed, cleared to hide it]. 5801C bit 6 = block, bit 5 = blink
    // [observed: setup items 23h and 24h, Display Setup's cursor style and
    // blink, set those bits, and the keyboard's cursor-select and
    // cursor-blink toggles flip them; so does the coax control register's
    // handler F000:9D67, bit 0 (blink) -> bit 5, bit 1 (reverse cursor) ->
    // bit 6]. 58016: the underline cursor's first (low
    // byte, bits 0-4) and last (high byte) scan line, counted from 1
    // [observed: the cell's last lines in every mode record; the numbering
    // is a hypothesis, see rows()].
    Cursor c;
    const uint16_t ctl = regs_[0x1C >> 1];
    c.visible = (ctl & 0x0080) != 0;
    c.block = (ctl & 0x0040) != 0;
    c.blink = (ctl & 0x0020) != 0;
    c.row = regs_[0x10 >> 1] >> 8;
    c.column = regs_[0x10 >> 1] & 0xFF;
    const unsigned first = regs_[0x16 >> 1] & 0x1F, last = (regs_[0x16 >> 1] >> 8) & 0x1F;
    c.first_line = first ? first - 1 : 0;
    c.last_line = last ? last - 1 : 0;
    return c;
}

bool Board3483::Display::blink_off() const { return (board_.frame_count_ / kBlinkFrames) & 1; }

uint16_t Board3483::Display::mmio_read16(uint32_t off) { return read_word(off); }
void Board3483::Display::mmio_write16(uint32_t off, uint16_t value) { write_word(off, value); }

uint8_t Board3483::Display::mmio_read8(uint32_t off) {
    const uint16_t w = read_word(off);
    return uint8_t(off & 1 ? w >> 8 : w);
}

void Board3483::Display::mmio_write8(uint32_t off, uint8_t value) {
    uint16_t w = regs_[(off & 0xFE) >> 1];
    w = off & 1 ? uint16_t((w & 0x00FF) | (value << 8)) : uint16_t((w & 0xFF00) | value);
    write_word(off, w);
}

uint8_t Board3483::Display::peek8(uint32_t off) { return mmio_read8(off); }

std::string Board3483::Display::describe() const {
    std::string s = "display controller 58000:\n";
    for (unsigned i = 0; i < 0x30; i += 2) s += format("  +%02x %04x%s", i, regs_[i >> 1], (i & 0xE) == 0xE ? "\n" : "");
    const Cursor c = cursor();
    s += format("%ux%u, %u-line cells; cursor %u,%u %s%s%s\n", rows(), columns(), cell_height(), c.row, c.column,
                c.visible ? "shown" : "hidden", c.block ? " block" : " underline", c.blink ? " blinking" : "");
    s += format("frame %llu, %s\n", (unsigned long long)board_.frame_count_, board_.in_vsync() ? "vsync" : "active");
    return s;
}

void Board3483::Display::render(Framebuffer& fb) {
    draw(fb);
    // Presented as the 5250 colour units are: a 4:3 tube with the same black
    // border, at two raster pixels per dot, with the beam's width tied to
    // a 1280-dot line so that it keeps its physical size (720 dots per line,
    // VGA text timing, are coarser than the 5250 tubes').
    const int w = fb.width, h = fb.height;
    std::vector<uint32_t> wide(size_t(w) * 2 * size_t(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) wide[size_t(y) * size_t(2 * w) + size_t(2 * x)] =
            wide[size_t(y) * size_t(2 * w) + size_t(2 * x + 1)] = fb.pixels[size_t(y) * size_t(w) + size_t(x)];
    fb.width = 2 * w;
    fb.pixels.swap(wide);
    fb.display_aspect = 4.0 / 3.0;
    fb.border_x = 0.06;
    fb.border_y = 0.077;
    fb.pixels_per_dot = 2;
    fb.dot_pitch = 1280.0 / double(fb.width);
}

void Board3483::Display::draw(Framebuffer& fb) {
    // 9 dots per cell [hypothesis], geometry from the mode registers; see
    // cell_dots().
    const unsigned cols = columns(), nrows = rows(), cell_h = cell_height();
    const unsigned cell_w = 9;
    fb.width = int(cols * cell_w);
    fb.height = int((nrows + 1) * cell_h);   // + the status row
    fb.pixels.assign(size_t(fb.width) * size_t(fb.height), colours::kBlack);
    const bool video_on = board_.coax_ && (board_.coax_->reg(0x10) & 1);
    if (!video_on) return;
    // The shades are the shared terminal colours (the owner compared photos
    // of a 3483 with the colour 5250 units); the colour number is 3270's.
    // 5801C bit 13 is cleared for the monochrome monitor class and for
    // Black-On-White emulation [observed], but also during the display test
    // [observed: 1080], so what it does is open; it is not modelled.
    static constexpr uint32_t kColour[8] = {colours::kGreen, colours::kBlue,      colours::kRed,
                                            colours::kPink,  colours::kGreen,     colours::kTurquoise,
                                            colours::kYellow, colours::kWhite};
    auto shade = [&](unsigned idx, bool) { return kColour[idx & 7]; };
    const bool blink = blink_off();
    const Cursor cur = cursor();
    // Rule line [observed: the rule-line routine D000:3632 writes 58012 =
    // row:column (FF = none) and 5801E bit 6 for Display Setup's
    // "Horizontal", bit 5 for "Vertical", both for "Cross"]. Drawn dotted on
    // the underline scan line and as the first dot column of the column
    // [hypothesis: photos show a dotted horizontal rule].
    const uint16_t rule = regs_[0x12 >> 1], rule_ctl = regs_[0x1E >> 1];
    const unsigned rule_row = (rule_ctl & 0x0040) ? (rule >> 8) : 0x100;
    const unsigned rule_col = (rule_ctl & 0x0020) ? (rule & 0xFF) : 0x100;
    const unsigned uline = cur.last_line < cell_h ? cur.last_line : cell_h - 1;
    FieldState f = initial_field();
    for (unsigned r = 0; r <= nrows; ++r) {
        const bool status = r == nrows;
        const uint32_t start = status ? kStatusRow : row_address(r);
        if (status) {
            f = FieldState();   // the status row has no fields of the screen's
            std::fill_n(&fb.pixels[size_t(r * cell_h) * size_t(fb.width)], size_t(fb.width), shade(1, false));
        }
        for (unsigned c = 0; c < cols; ++c) {
            const uint8_t ch = board_.bus_.peek8(start + 2 * c), eab = board_.bus_.peek8(start + 2 * c + 1);
            const FieldState before = f;
            const bool at_cursor = cur.visible && cur.row == r && cur.column == c && !(cur.blink && blink);
            unsigned cursor_idx = cell_colour(ch, eab, before);
            if (!cursor_idx) cursor_idx = 4;   // over a field attribute: the base colour [hypothesis]
            for (unsigned y = status ? 1 : 0; y < cell_h; ++y) {
                f = before;
                unsigned idx;
                uint16_t bits = cell_dots(ch, eab, f, y, cell_h, idx, blink);
                if (!idx) idx = cursor_idx;
                if (at_cursor) {
                    // A block cursor reverses the whole cell; the underline
                    // cursor lights its lines in the cell's colour.
                    if (cur.block) bits = uint16_t(~bits & 0xFF80);
                    else if (y >= cur.first_line && y <= cur.last_line) bits = 0xFF80;
                }
                uint32_t* row = &fb.pixels[size_t(r * cell_h + y) * size_t(fb.width) + c * cell_w];
                const uint32_t ink = shade(idx, f.high);
                for (unsigned x = 0; x < cell_w; ++x)
                    if (bits & (0x8000u >> x)) row[x] = ink;
                if (!status && ((r == rule_row && y == uline) || c == rule_col)) {
                    const uint16_t rd = r == rule_row && y == uline ? uint16_t(0xAA80) : uint16_t(0x8000);
                    for (unsigned x = 0; x < cell_w; ++x)
                        if (rd & (0x8000u >> x)) row[x] = shade(7, false);
                }
            }
        }
    }
    // 5801A bit 15: the whole picture reversed, dark on light [observed: the
    // firmware sets it, with bit 1, for Black-On-White emulation (setup
    // byte 3CD = 0); hypothesis for its effect and for drawing it as black
    // on white].
    if (regs_[0x1A >> 1] & 0x8000)
        for (uint32_t& p : fb.pixels) p = p == colours::kBlack ? colours::kGreyBright : colours::kBlack;
}

// The coax adapter (58100) is in board_3483_coax.cpp.

// --- system control (58400) --------------------------------------------------

Board3483::System::System(Board3483& board) : Device("system-3483"), board_(board) { board_.system_ = this; }

void Board3483::System::reset() {
    if (audio_ && (regs_[0x02 >> 1] & 0x0080)) audio_->tone(false, 0.0);
    regs_.fill(0);
    channels_ = {};
    tx_left_ = rx_left_ = hold_left_ = 0;
    rx_active_ = false;
    us_acc_ = 0;
    if (kbd_) kbd_->power_on();
}

void Board3483::System::attach_keyboard(Set3Keyboard* kbd, uint64_t clock_hz) {
    kbd_ = kbd;
    clock_hz_ = clock_hz ? clock_hz : 1;
    byte_cycles_ = clock_hz_ / 1000;   // about 1 ms per 11-bit frame [hypothesis, keyboard clock ~11 kHz]
}

// Sending to the keyboard [observed: svc_113, D000:3116]: the firmware
// writes byte | odd parity (bit 8) | stop (bit 9) with bit 10 low, which
// pulls the data line (request to send), clears the control register and
// polls control bit 2 until the frame is out; data then reads 0 when the
// keyboard acknowledged it.
void Board3483::System::data_write(unsigned channel, uint16_t value) {
    Channel& ch = channels_[channel & 1];
    ch.data = ch.drive = value;
    if ((channel & 1) == 0 && kbd_ && !(value & 0x0400) && !(ch.control & 0x0080)) {
        tx_byte_ = uint8_t(value);
        tx_left_ = byte_cycles_ ? byte_cycles_ : 1;
    }
}

void Board3483::System::tick(uint64_t cycles) {
    if (!kbd_) return;
    us_acc_ += cycles * 1000000;
    const uint64_t us = us_acc_ / clock_hz_;
    us_acc_ %= clock_hz_;
    if (us) kbd_->advance_us(us);
    Channel& a = channels_[0];
    if (tx_left_) {
        if (cycles < tx_left_) {
            tx_left_ -= cycles;
            return;
        }
        tx_left_ = 0;
        LOG_DEBUG("kbd", "host -> keyboard %02X", tx_byte_);
        kbd_->host_write(tx_byte_);
        a.data = 0;              // acknowledged
        a.control |= 0x0004;     // frame out [observed: polled bit]
        // The keyboard answers a few milliseconds later; the firmware sets
        // up its receive wait (clearing the status) in between [observed:
        // an immediate answer is lost].
        hold_left_ = 3 * byte_cycles_;
        return;
    }
    if (hold_left_) {
        hold_left_ = cycles < hold_left_ ? hold_left_ - cycles : 0;
        return;
    }
    // The keyboard sends while the channel is armed and the CPU is not
    // driving the lines.
    if (!a.armed || (a.control & 0x0080) || !kbd_->has_output()) {
        rx_active_ = false;
        return;
    }
    if (!rx_active_) {
        rx_active_ = true;
        rx_left_ = byte_cycles_;
    }
    if (cycles < rx_left_) {
        rx_left_ -= cycles;
        return;
    }
    rx_active_ = false;
    const uint8_t b = kbd_->read_output();
    LOG_DEBUG("kbd", "keyboard -> host %02X", b);
    receive(0, b);
}

uint16_t Board3483::System::sense() const {
    // Bits 1-0 = 11: normal power-on (no test fixture) [observed decoder].
    // Bit 2: vertical sync, low during the pulse [hypothesis, strong]. Bits 8-11: monitor sense
    // lines, line 1 in bit 11, each per its two-bit code [observed decoder].
    uint16_t v = 0x0003;
    if (!board_.in_vsync()) v |= 0x0004;   // 1 during the frame, 0 in the sync pulse [observed: POST windows]
    const uint16_t drive = board_.display_ ? board_.display_->reg(0x26) : 0;
    for (unsigned line = 0; line < 4; ++line) {
        const unsigned code = (board_.monitor_sense_ >> (6 - 2 * line)) & 3;
        bool level = false;
        switch (code) {
        case 0: level = false; break;
        case 1: level = true; break;
        case 2: level = (drive & 0x20) != 0; break;
        case 3: level = (drive & 0x01) != 0; break;
        }
        if (level) v |= uint16_t(0x0800 >> line);
    }
    // Bits 12-15 written: each pulls its own sense line (bit 8-11) low; no
    // line is tied to another [observed: the loopback check C000:B82F drives
    // one line at a time and expects only that line to drop for an
    // ordinary monitor; hypothesis for the wiring].
    v = uint16_t(v & ~((regs_[0x0E >> 1] >> 4) & 0x0F00));
    return v;
}

uint16_t Board3483::System::read_word(unsigned off) const {
    off &= 0xFE;
    switch (off) {
    case 0x08: return channels_[0].data;
    case 0x12: return channels_[1].data;
    case 0x06:
    case 0x10: {
        // Control read: bit 6 is the data line, driven from data register
        // bit 10; the clock line (bit 5) is not driven and reads low
        // [observed: POST tests 14/15, C000:B537-B59C].
        const Channel& ch = channels_[off == 0x06 ? 0 : 1];
        return uint16_t((ch.control & ~0x0060) | (ch.drive & 0x0400 ? 0x0040 : 0));
    }
    case 0x0E: return sense();
    default: return regs_[off >> 1];
    }
}

// 58402, the alarm [observed: svc_177 D000:3478 sounds the alarm with bit
// 7 set and bits 6-0 = setup byte 3C7 (Display Setup's alarm volume, item
// 28h) for 8 ticks, then clears bit 7; a keystroke click is bit 10 set and
// cleared with bits 6-0 = setup byte 3C8 (the clicker volume, item 29h);
// POST's beeps (C000:83CD) set bit 7 with a pattern byte; reset writes
// 809B and the end of the early RAM and ROM checks 8000, so the alarm
// sounds until they pass]. Bits 6-0 as volume is a hypothesis (the pitch
// is not set anywhere: the sink's default); bit 15 is always set after the
// checks (hypothesis: speaker enable).
void Board3483::System::alarm_write(uint16_t value, uint16_t old) {
    if ((value ^ old) & 0x0080) {
        LOG_DEBUG("sound", "alarm %s (level %02X)", (value & 0x80) ? "on" : "off", value & 0x7F);
        if (audio_) audio_->tone((value & 0x0080) != 0, 0.0);
    }
    if ((value & ~old) & 0x0400) {
        LOG_TRACE("sound", "click (level %02X)", value & 0x7F);
        if (audio_) audio_->click();
    }
}

void Board3483::System::write_word(unsigned off, uint16_t value) {
    off &= 0xFE;
    const uint16_t old = regs_[off >> 1];
    regs_[off >> 1] = value;
    switch (off) {
    case 0x02: alarm_write(value, old); break;
    case 0x06:
    case 0x10: control_write(off == 0x06 ? 0 : 1, value); break;
    case 0x08: data_write(0, value); break;
    case 0x12: data_write(1, value); break;
    default: break;
    }
}

// Serial channel control (58406 for A, 58410 for B) [observed: POST tests
// 12/13, C000:B3AB-B46F]: bit 2 arms the receiver; with bit 7 set the CPU
// drives the lines itself, bit 6 = data, bit 5 = clock, and the receiver
// takes the data line on each falling clock edge, least significant bit
// first. After 11 bits (start, 8 data, parity, stop) the frame shifted
// right by one (data in bits 0-7, parity bit 8, stop bit 9) is latched in
// the data register and the channel interrupts [observed: 074A -> 03A5].
void Board3483::System::control_write(unsigned channel, uint16_t value) {
    Channel& ch = channels_[channel & 1];
    const uint16_t old = ch.control;
    ch.control = value;
    // Arming starts a new frame, and so does the CPU taking the lines; the
    // write that does so is not a clock edge [observed: POST re-arms with
    // the clock line dropping between its two test frames].
    const bool arming = (value & 0x0004) && !ch.armed;
    if (value & 0x0004) ch.armed = true;
    if (arming) {
        // Re-arming drops the channel's previous "received" status
        // [hypothesis: the firmware enables the interrupt right after
        // re-arming without clearing it, and a stale one would read FFFF].
        if ((channel & 1) == 0) board_.status1_ = uint16_t(board_.status1_ & ~0x0010);
        else board_.status2_ = uint16_t(board_.status2_ & ~0x8000);
        board_.update_irq();
    }
    if (!(value & 0x0080)) return;
    if (arming || !(old & 0x0080)) {
        ch.shift = 0;
        ch.bits = 0;
        return;
    }
    const bool falling = (old & 0x0020) && !(value & 0x0020);
    if (!falling) return;
    if (value & 0x0040) ch.shift |= uint16_t(1u << ch.bits);
    if (++ch.bits == 11) {
        frame_received(channel, ch.shift);
        ch.shift = 0;
        ch.bits = 0;
    }
}

void Board3483::System::frame_received(unsigned channel, uint16_t frame) {
    Channel& ch = channels_[channel & 1];
    ch.data = uint16_t(frame >> 1);
    if (!ch.armed) return;
    ch.armed = false;
    if ((channel & 1) == 0) board_.raise1(0x0010);
    else board_.raise2(0x8000);
}

bool Board3483::System::receive(unsigned channel, uint8_t byte) {
    Channel& ch = channels_[channel & 1];
    if (!ch.armed) return false;
    bool odd = false;
    for (uint8_t b = byte; b; b &= uint8_t(b - 1)) odd = !odd;
    // start 0, data, odd parity over data + parity, stop 1
    const uint16_t frame = uint16_t((byte << 1) | (odd ? 0 : 0x200) | 0x400);
    frame_received(channel, frame);
    return true;
}

uint16_t Board3483::System::mmio_read16(uint32_t off) { return read_word(off); }
void Board3483::System::mmio_write16(uint32_t off, uint16_t value) { write_word(off, value); }

uint8_t Board3483::System::mmio_read8(uint32_t off) {
    const uint16_t w = read_word(off);
    return uint8_t(off & 1 ? w >> 8 : w);
}

void Board3483::System::mmio_write8(uint32_t off, uint8_t value) {
    const uint16_t cur = regs_[(off & 0xFE) >> 1];
    write_word(off, off & 1 ? uint16_t((cur & 0x00FF) | (value << 8)) : uint16_t((cur & 0xFF00) | value));
}

uint8_t Board3483::System::peek8(uint32_t off) { return mmio_read8(off); }

std::string Board3483::System::describe() const {
    return format("system 58400: indicator %04x progress %04x sense %04x channel A %s %04x, B %s %04x\n", regs_[1],
                  regs_[5], sense(), channels_[0].armed ? "armed" : "idle", channels_[0].data,
                  channels_[1].armed ? "armed" : "idle", channels_[1].data);
}

} // namespace em34xx
