// SPDX-License-Identifier: GPL-3.0-or-later
// The 3483 board devices (hw/board_3483.hpp) as the firmware programs them:
// display geometry, cursor, highlighting, the alarm and the system ports.
#include "bus/bus.hpp"
#include "hw/board_3483.hpp"
#include "hw/i80186_peripherals.hpp"
#include "hw/terminal_colours.hpp"
#include "machine/peripherals.hpp"

#include <doctest/doctest.h>

using namespace em34xx;

namespace {

constexpr uint64_t kClock = 10000000;

// A board with RAM where the firmware keeps the regen buffer (0:0000), the
// character sets (SRAM 40000) and the row lists (50000), and the three
// register devices mapped as the model maps them.
struct Rig {
    Bus bus{20};
    I80186Peripherals pcb{bus};
    Board3483 board{bus, pcb, kClock, 0x44};
    Board3483::Display display{board};
    Board3483::Coax coax{board};
    Board3483::System system{board};
    Rig() {
        bus.map_ram("DRAM", 0x00000, 0x40000);
        bus.map_ram("SRAM", 0x40000, 0x10000);
        bus.map_ram("block RAM", 0x50000, 0x800);
        bus.map_mmio("display", 0x58000, 0x100, &display);
        bus.map_mmio("coax", 0x58100, 0x100, &coax);
        bus.map_mmio("system", 0x58400, 0x100, &system);
        display.reset();
        coax.reset();
        system.reset();
    }
    void w16(uint32_t a, uint16_t v) { bus.mem_write16(a, v); }
    // The 24x80 mode record's geometry, a linear screen at 0:1000, video on.
    void mode_24x80() {
        w16(0x58000, 0x634F);
        w16(0x58006, 0x1C17);
        w16(0x5800A, 0x0812);
        w16(0x58016, 0x1291);
        w16(0x5801A, 0x0040);
        w16(0x5801C, 0x1000);
        w16(0x58110, 0x0001);
    }
    void glyph(uint8_t ch, unsigned line, uint8_t main_byte) { bus.poke8(0x48000 + ch * 32u + line, main_byte); }
    void cell(unsigned row, unsigned col, uint8_t ch, uint8_t eab) {
        const uint32_t a = 0x1000 + 2 * (row * 80 + col);
        bus.poke8(a, ch);
        bus.poke8(a + 1, eab);
    }
    Framebuffer shot() {
        Framebuffer fb;
        display.render(fb);
        return fb;
    }
    static uint32_t px(const Framebuffer& fb, unsigned x, unsigned y) { return fb.pixels[size_t(y) * fb.width + x]; }
};

} // namespace

TEST_CASE("3483 display geometry follows the mode registers") {
    Rig r;
    r.mode_24x80();
    CHECK(r.display.rows() == 24);
    CHECK(r.display.columns() == 80);
    CHECK(r.display.cell_height() == 18);
    Framebuffer fb = r.shot();
    CHECK(fb.width == 720);
    CHECK(fb.height == 25 * 18);   // with the status row

    // 32x80 (record 1) and 27x132 (record 6).
    r.w16(0x58006, 0x1C1F);
    r.w16(0x5800A, 0x0E0D);
    CHECK(r.display.rows() == 32);
    CHECK(r.display.cell_height() == 13);
    r.w16(0x58000, 0xA583);
    r.w16(0x58006, 0x1C1A);
    r.w16(0x5800A, 0x0710);
    fb = r.shot();
    CHECK(r.display.columns() == 132);
    CHECK(fb.width == 132 * 9);
    CHECK(fb.height == 28 * 16);
}

TEST_CASE("3483 cursor: position, underline lines, block and blink") {
    Rig r;
    r.mode_24x80();
    r.w16(0x58010, 0x0D09);   // row 13, column 9 (the display test's)
    Board3483::Display::Cursor c = r.display.cursor();
    CHECK_FALSE(c.visible);
    CHECK(c.row == 13);
    CHECK(c.column == 9);
    CHECK(c.first_line == 16);
    CHECK(c.last_line == 17);
    const unsigned x = 9 * 9 + 4, top = 13 * 18;
    CHECK(Rig::px(r.shot(), x, top + 17) == colours::kBlack);

    r.w16(0x5801C, 0x1080);   // shown, underline
    Framebuffer fb = r.shot();
    CHECK(Rig::px(fb, x, top + 15) == colours::kBlack);
    CHECK(Rig::px(fb, x, top + 16) != colours::kBlack);
    CHECK(Rig::px(fb, x, top + 17) != colours::kBlack);

    r.w16(0x5801C, 0x10C0);   // block: the whole cell
    fb = r.shot();
    CHECK(Rig::px(fb, x, top) != colours::kBlack);
    CHECK(Rig::px(fb, x, top + 8) != colours::kBlack);

    // Blink: dark in every other phase.
    r.w16(0x5801C, 0x10E0);
    bool seen_on = false, seen_off = false;
    for (int i = 0; i < 4; ++i) {
        (Rig::px(r.shot(), x, top + 8) != colours::kBlack ? seen_on : seen_off) = true;
        r.board.advance(kClock / 60 * 32);
    }
    CHECK(seen_on);
    CHECK(seen_off);
}

TEST_CASE("3483 cells: field character set, underscore style, reverse and blink") {
    Rig r;
    r.mode_24x80();
    // Glyph B9: a line in the APL/GE set, 'Z' in the base set.
    r.bus.poke8(0x4A000 + 0xB9 * 32 + 9, 0x7F);
    r.glyph(0xB9, 2, 0x7F);
    // A field whose EAB selects the GE set and white; the next cell has no
    // EAB of its own.
    r.cell(0, 0, 0xC1, 0x39);
    r.cell(0, 1, 0xB9, 0x00);
    Framebuffer fb = r.shot();
    CHECK(Rig::px(fb, 9 + 4, 9) == colours::kWhite);
    CHECK(Rig::px(fb, 9 + 4, 2) == colours::kBlack);

    // Underscore (EAB 11) on the cursor lines: solid with 58016 bit 7,
    // dashed without.
    r.cell(1, 0, 0xC0, 0x00);
    r.cell(1, 1, 0x40, 0xF8);   // blank, underscore, white
    fb = r.shot();
    CHECK(Rig::px(fb, 9, 18 + 17) == colours::kWhite);
    r.w16(0x58016, 0x1211);
    fb = r.shot();
    CHECK(Rig::px(fb, 9, 18 + 17) == colours::kBlack);
    CHECK(Rig::px(fb, 9 + 4, 18 + 17) == colours::kWhite);

    // Reverse (EAB 10): the cell in its colour, the glyph dark.
    r.glyph(0x41, 5, 0x20);   // one dot, the cell's second (main bit 6 is the first)
    r.cell(2, 1, 0x41, 0xB0);   // reverse, yellow
    fb = r.shot();
    CHECK(Rig::px(fb, 9 + 0, 36 + 5) == colours::kYellow);
    CHECK(Rig::px(fb, 9 + 1, 36 + 5) == colours::kBlack);

    // Blink (EAB 01): lit, then dark half a second later.
    r.cell(3, 1, 0x41, 0x78);
    CHECK(Rig::px(r.shot(), 9 + 1, 54 + 5) == colours::kWhite);
    r.board.advance(kClock / 60 * 32);
    CHECK(Rig::px(r.shot(), 9 + 1, 54 + 5) == colours::kBlack);
}

TEST_CASE("3483 rule line and Black-On-White") {
    Rig r;
    r.mode_24x80();
    r.w16(0x58012, 0x0503);   // row 5, column 3
    CHECK(Rig::px(r.shot(), 3 * 9, 0) == colours::kBlack);
    r.w16(0x5801E, 0x0020);   // vertical
    Framebuffer fb = r.shot();
    CHECK(Rig::px(fb, 3 * 9, 0) == colours::kWhite);
    CHECK(Rig::px(fb, 3 * 9, 5 * 18 + 17) == colours::kWhite);
    r.w16(0x5801E, 0x0040);   // horizontal, dotted on the underline line
    fb = r.shot();
    CHECK(Rig::px(fb, 3 * 9, 0) == colours::kBlack);
    CHECK(Rig::px(fb, 0, 5 * 18 + 17) == colours::kWhite);
    CHECK(Rig::px(fb, 1, 5 * 18 + 17) == colours::kBlack);

    r.w16(0x5801E, 0x0000);
    r.w16(0x5801A, 0x8040);   // Black-On-White
    fb = r.shot();
    CHECK(Rig::px(fb, 100, 100) != colours::kBlack);
}
