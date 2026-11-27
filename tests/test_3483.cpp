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
    Board3483 board{bus, &pcb, kClock, 0x44};
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
        display.draw(fb);
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
    {
        // Presented as the 5250 colour units: two pixels per dot, 4:3, bordered.
        Framebuffer shown;
        r.display.render(shown);
        CHECK(shown.width == 1440);
        CHECK(shown.pixels_per_dot == 2);
        CHECK(shown.border_x > 0.0);
    }
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

TEST_CASE("3483 cursor: an underline cursor shows in an underscored field") {
    // A 3270 cursor sits in an input field, which is usually underscored
    // on the cursor's own lines: the cursor reverses those lines, so the
    // underscore has a gap where the cursor is.
    Rig r;
    r.mode_24x80();
    r.cell(5, 9, 0xC0, 0xF8);   // field attribute: underscore, white
    for (unsigned c = 10; c < 20; ++c) r.cell(5, c, 0x40, 0x00);
    r.w16(0x58010, 0x050C);     // row 5, column 12
    r.w16(0x5801C, 0x1080);     // shown, underline, steady
    const Framebuffer fb = r.shot();
    const unsigned top = 5 * 18;
    CHECK(Rig::px(fb, 11 * 9 + 4, top + 16) == colours::kWhite);   // the underscore beside it
    CHECK(Rig::px(fb, 12 * 9 + 4, top + 16) == colours::kBlack);   // the cursor
    CHECK(Rig::px(fb, 12 * 9 + 4, top + 17) == colours::kBlack);
    CHECK(Rig::px(fb, 13 * 9 + 4, top + 17) == colours::kWhite);
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

namespace {
struct RecordingSink : AudioSink {
    int tones_on = 0, tones_off = 0, clicks = 0;
    void tone(bool on, double) override { (on ? tones_on : tones_off)++; }
    void speaker(bool) override {}
    void click() override { ++clicks; }
};
} // namespace

TEST_CASE("3483 alarm and clicker on 58402") {
    Rig r;
    RecordingSink sink;
    r.system.set_audio(&sink);
    r.w16(0x58402, 0x809B);   // reset: the alarm sounds...
    CHECK(sink.tones_on == 1);
    r.w16(0x58402, 0x8000);   // ...until the early checks pass
    CHECK(sink.tones_off == 1);
    r.w16(0x58402, 0x8085);   // the alarm at a setup level
    r.w16(0x58402, 0x8005);
    CHECK(sink.tones_on == 2);
    CHECK(sink.tones_off == 2);
    r.w16(0x58402, 0x8403);   // a click: bit 10 pulsed
    r.w16(0x58402, 0x8003);
    CHECK(sink.clicks == 1);
    CHECK(sink.tones_on == 2);
}

TEST_CASE("3483 memory engine: bit 6 leaves the operation, search finds the field attribute") {
    Rig r;
    // Copy (op 00) with bit 6 set, as the display test starts it (C1): the
    // coax buffer's top-left cell to the regen buffer.
    r.bus.poke8(0xC0A0, 0xA0);
    r.bus.poke8(0xC0A1, 0x20);
    r.w16(0x5811C, 1);
    r.w16(0x58118, 0xC0A0);
    r.w16(0x5811A, 0x1000);
    r.w16(0x58110, 0x00C1);
    CHECK(r.bus.peek8(0x1000) == 0xA0);
    CHECK(r.bus.peek8(0x1001) == 0x20);
    CHECK((r.bus.mem_read16(0x58110) & 0x80) == 0);

    // Search (op 30, F1 with bit 6): back to the nearest field attribute,
    // a character C0-FF, whatever its attribute byte.
    for (uint32_t a = 0xC000; a < 0xC010; a += 2) {
        r.bus.poke8(a, 0x20);
        r.bus.poke8(a + 1, 0x00);
    }
    r.bus.poke8(0xC004, 0xE8);
    r.bus.poke8(0xC005, 0x20);
    r.w16(0x5811C, 8);
    r.w16(0x58118, 0xC00E);
    r.w16(0x58110, 0x00F1);
    CHECK(r.bus.mem_read16(0x58118) == 0xC004);
    // None in range: the pointer ends count cells back (the firmware's
    // "no field" test, F000:BB28).
    r.bus.poke8(0xC004, 0x20);
    r.w16(0x58118, 0xC00E);
    r.w16(0x58110, 0x00B0);
    CHECK(r.bus.mem_read16(0x58118) == uint16_t(0xC00E - 2 * 8));
}

TEST_CASE("3483 status row: drawn from 0:0A00, not the local buffer at 0:0800") {
    // The runtime copies the operator information area it builds (the
    // controller's coax cells 0-79 on line, its local buffer 0:0800 in
    // setup) to 0:0A00 [observed]; that is the row shown under the screen.
    Rig r;
    r.mode_24x80();
    r.glyph(0xB3, 5, 0xFF);   // a bar on glyph row 5
    const unsigned x = 2 * 9 + 3, y = 24 * 18 + 5;   // status row, cell 2
    r.bus.poke8(0x0800 + 4, 0xB3);   // only in the local buffer: not shown
    r.bus.poke8(0x0800 + 5, 0x38);
    CHECK(Rig::px(r.shot(), x, y) == colours::kBlack);
    r.bus.poke8(0x0A00 + 4, 0xB3);
    r.bus.poke8(0x0A00 + 5, 0x38);   // white
    CHECK(Rig::px(r.shot(), x, y) == colours::kWhite);
}

TEST_CASE("system sense: monitor sense lines on the 3483, a display ID on the 3472") {
    // 3483: sense code 44 (lines 1 and 3 open) in bits 11-8; bits 7-5 clear.
    Rig r;
    const uint16_t v3483 = r.bus.mem_read16(0x5840E);
    CHECK((v3483 & 0x0F00) == 0x0A00);
    CHECK((v3483 & 0x00E0) == 0);
    CHECK((v3483 & 0x0003) == 0x0003);   // normal power-on

    // 3472: no sense lines; the strap byte's bits 7-5 read in bits 7-5.
    Bus bus{20};
    CoaxBoardLayout l;
    l.monitor_sense_lines = false;
    Board3483 board{bus, nullptr, kClock, 0x40, l};
    Board3483::System system{board};
    bus.map_mmio("system", l.system, 0x100, &system);
    system.reset();
    const uint16_t v3472 = bus.mem_read16(l.system + 0x0E);
    CHECK((v3472 & 0x00E0) == 0x0040);   // 01x: display type 2
    CHECK((v3472 & 0x0F00) == 0);
    CHECK((v3472 & 0x0003) == 0x0003);
}
