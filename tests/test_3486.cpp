// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware the 3486 family adds to the 3477 design: the second serial port,
// the third Twinax station channel and the display's sync polarity bit.
#include "bus/bus.hpp"
#include "models/alignment_shim_3486.hpp"
#include "hw/aux_port.hpp"
#include "models/board_3486.hpp"
#include "hw/i2c_eeprom.hpp"
#include "hw/display_controller.hpp"
#include "hw/keyboard_interface.hpp"
#include "hw/system_gate_array.hpp"
#include "hw/twinax_engine.hpp"
#include "models/board_3477.hpp"
#include "models/model.hpp"

#include <doctest/doctest.h>

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

using namespace em34xx;
using Src = GateArrayInterrupts::Source;

namespace {
constexpr uint64_t kClock = 7500000;

std::unique_ptr<SystemGateArray> make_system(unsigned link_channels = kLinkChannels3486) {
    auto k = std::make_unique<KeyboardInterface>(kClock, KeyboardType::Terminal122, 0xBF, 0xBF);
    auto sys = std::make_unique<SystemGateArray>(kClock, uint8_t(0x42), link_channels, std::move(k));
    sys->reset();
    return sys;
}
} // namespace

TEST_CASE("3486 second serial port: manual-clock wrap and its interrupt") {
    auto sys = make_system();
    GateArrayInterrupts& ic = sys->interrupts();
    ic.set_three_channel_layout();
    AuxPort aux;
    aux.connect(&ic);
    aux.reset();
    CHECK((aux.read(1) & 0x80) == 0);   // layout bit

    for (uint8_t byte : {uint8_t(0xAB), uint8_t(0x54)}) {
        aux.write(1, 0x01);
        aux.write(2, 0xFF);
        aux.write(0, byte);
        for (int clock = 0; clock < 11; ++clock) {
            CHECK((aux.read(1) & 0x40) == 0);
            aux.write(1, 0x01);
            aux.write(1, 0x03);
        }
        CHECK((aux.read(1) & 0x40) != 0);   // byte received
        CHECK(aux.read(0) == byte);
        // AB has five ones, 54 three: the odd-parity bit is 0 for both...
        CHECK((aux.read(1) & 0x01) == 0);
        CHECK(ic.line(Src::Aux));
        CHECK_FALSE(ic.intr_asserted());     // not enabled yet
        sys->mmio_write8(0x07, 0x02);        // 20007 bit 1
        REQUIRE(ic.intr_asserted());
        CHECK(ic.acknowledge() == 0x0E);
        aux.write(2, 0xFF);
        CHECK_FALSE(ic.line(Src::Aux));
        CHECK((aux.read(1) & 0x40) == 0);
        sys->mmio_write8(0x07, 0x00);
    }
    // ...and 1 for a byte with an even number of ones.
    aux.write(0, 0x03);
    for (int clock = 0; clock < 11; ++clock) {
        aux.write(1, 0x01);
        aux.write(1, 0x03);
    }
    CHECK((aux.read(1) & 0x41) == 0x41);

    // A byte sent with the line released goes nowhere: no device.
    aux.write(2, 0xFF);
    aux.write(0, 0xFF);
    aux.write(1, 0x02);
    CHECK((aux.read(1) & 0x40) == 0);
    CHECK_FALSE(ic.line(Src::Aux));
}

TEST_CASE("3486 third Twinax channel: registers, programs at 0200, descriptor at 01A0") {
    Bus bus{20};
    bus.map_ram("ram", 0, 0x10000);
    TwinaxEngine eng(bus, 3, kClock, kLinkLayout3486);
    auto sys = make_system();
    GateArrayInterrupts& ic = sys->interrupts();
    eng.set_raise([&ic](unsigned ch, uint8_t bits) {
        if (ch == 2) ic.request2(bits);
        else ic.request(uint16_t(bits << (8 * ch)));
    });
    eng.set_pending([&ic](unsigned ch) {
        return uint8_t((ch == 2 ? ic.requests2() : uint8_t(ic.requests() >> (8 * ch))) & 0x7F);
    });
    eng.reset();
    TwinaxRegisters regs(eng);
    regs.reset();
    sys->set_twinax_port(&regs);
    auto w = [&](uint32_t off, uint8_t v) { sys->mmio_write8(off, v); };

    // Queue program for the third channel only, where the firmware puts it.
    uint32_t a = 0x0200;
    const std::initializer_list<uint8_t> queue_program = {0x55, 0x22, 0x56, 0x20, 0x57, 0x21, 0x5F, 0x23, 0x82,
                                                          0xA2, 0x40, 0x36, 0x41, 0x37, 0x42, 0x35, 0x84};
    for (uint8_t b : queue_program) bus.poke8(a++, b);
    bus.poke8(0x01A5, 0x80);   // free bytes in the queue
    bus.poke8(0x01A6, 0xFF);   // queue write address 3FFF
    bus.poke8(0x01A7, 0x3F);

    w(0x08, 0x2A);
    w(0x1A, 0x01);
    CHECK(eng.station_address(2) == 1);
    w(0x09, 0x79);
    w(0x1B, 0x07);
    CHECK(eng.channel_enabled(2));
    CHECK_FALSE(eng.channel_enabled(0));
    w(0x24, 0x01);
    w(0x25, 0x42);
    w(0x26, 0x0B);
    w(0x27, 0xEE);
    w(0x09, 0xF9);
    w(0x15, 0x48);
    CHECK(bus.peek8(0x01A5) == 0x7E);
    CHECK(bus.peek8(0x01A6) == 0x01);
    CHECK(bus.peek8(0x01A7) == 0x40);
    CHECK(bus.peek8(0x4000) == 0xEE);

    // Its events land in 2000D, masked by 2000F, and clear by writing ones.
    CHECK(ic.requests() == 0);
    CHECK(sys->mmio_read8(0x0D) != 0);
    w(0x0F, 0x5A);
    CHECK(sys->mmio_read8(0x0F) == 0x5A);
    w(0x0D, 0xFF);
    CHECK(sys->mmio_read8(0x0D) == 0);

    // Control register 2000B: bit 7 drops busy.
    CHECK(eng.busy(2));
    w(0x0B, 0x80);
    CHECK_FALSE(eng.busy(2));
    w(0x1B, 0x06);
    CHECK_FALSE(eng.channel_enabled(2));
}

TEST_CASE("3477 gate array keeps 2000B-2000F and 2001A/B as plain storage") {
    auto sys = make_system(kLinkChannels3477);
    for (uint32_t off : {0x0Bu, 0x0Du, 0x0Fu, 0x1Au, 0x1Bu}) {
        sys->mmio_write8(off, 0xA5);
        CHECK(sys->mmio_read8(off) == 0xA5);
    }
}

// A display of each family on a bus of its own.
struct DisplayRig {
    Bus bus{20};
    TerminalOptions opts;
    DisplayController disp;

    explicit DisplayRig(const DisplayLayout& layout, bool colour = false) : disp(bus, layout, colour, true) {
        bus.map_ram("ram", 0, 0x20000);
        bus.map_mmio("display", 0x20100, 0x100, &disp);
        disp.set_clock_hz(kClock);
    }
    uint64_t retrace_cycles() {
        while (!disp.in_retrace()) disp.tick(1);
        uint64_t n = 0;
        while (disp.in_retrace()) {
            disp.tick(1);
            ++n;
        }
        return n;
    }
    // Start a signature measurement with 2012F = 10 then 90 at the start of
    // a retrace and read the result one field later.
    uint8_t measure() {
        while (disp.in_retrace()) disp.tick(16);
        while (!disp.in_retrace()) disp.tick(16);
        bus.mem_write8(0x2012F, 0x10);
        bus.mem_write8(0x2012F, 0x90);
        while (disp.in_retrace()) disp.tick(16);
        while (!disp.in_retrace()) disp.tick(16);
        const uint8_t v = uint8_t(bus.mem_read8(0x2012F) >> 4);
        bus.mem_write8(0x2012F, 0x00);
        return v;
    }
};

TEST_CASE("3486 display: retrace length and the sync polarity bit") {
    DisplayRig a(kDisplayLayout3477), b(kDisplayLayout3486);
    a.retrace_cycles();   // align to a whole retrace
    CHECK(a.retrace_cycles() == a.disp.cycles_per_frame() / 90);
    b.retrace_cycles();
    CHECK(b.retrace_cycles() == b.disp.cycles_per_frame() / 140);

    // 20128 bit 3 inverts the flag only when the board has that bit.
    for (DisplayRig* r : {&a, &b}) {
        while (!r->disp.in_retrace()) r->disp.tick(1);
        r->bus.mem_write8(0x20128, 0x18);
    }
    CHECK((a.bus.mem_read8(0x2012F) & 0x08) != 0);
    CHECK((b.bus.mem_read8(0x2012F) & 0x08) == 0);
    b.bus.mem_write8(0x20128, 0x10);
    CHECK((b.bus.mem_read8(0x2012F) & 0x08) != 0);
}

TEST_CASE("3486 display: the picture is on with 20128 bit 6") {
    // Bit 4 on the 3477, bit 6 on the 3486 family (whose colour tube never
    // sets bit 4).
    DisplayRig a(kDisplayLayout3477), b(kDisplayLayout3486);
    for (DisplayRig* r : {&a, &b}) {
        r->bus.mem_write8(0x20114, 25);
        r->bus.mem_write8(0x20115, 24);
        r->bus.mem_write8(0x20128, 0x10);
    }
    CHECK(a.disp.enabled());
    CHECK_FALSE(b.disp.enabled());
    for (DisplayRig* r : {&a, &b}) r->bus.mem_write8(0x20128, 0x40);
    CHECK_FALSE(a.disp.enabled());
    CHECK(b.disp.enabled());
    b.bus.mem_write8(0x20128, 0x18);
    CHECK_FALSE(b.disp.enabled());
}

TEST_CASE("3486 display: the cursor is switched by 20127 bit 6") {
    DisplayRig a(kDisplayLayout3477), b(kDisplayLayout3486);

    // 3477: 20128 bit 7.
    a.bus.mem_write8(0x20127, 0x70);
    CHECK_FALSE(a.disp.cursor_on());
    a.bus.mem_write8(0x20128, 0xD0);
    CHECK(a.disp.cursor_on());

    // 3486 family: the firmware never sets that bit; it writes 20127 with
    // bit 6 to show the cursor and without it to hide it.
    b.bus.mem_write8(0x20128, 0xD8);
    CHECK_FALSE(b.disp.cursor_on());
    b.bus.mem_write8(0x20128, 0x58);
    b.bus.mem_write8(0x20127, 0x70);
    CHECK(b.disp.cursor_on());
    b.bus.mem_write8(0x20127, 0x30);
    CHECK_FALSE(b.disp.cursor_on());
}

TEST_CASE("3486 display: writes to the screen are tracked and dots are widened by half") {
    DisplayRig a(kDisplayLayout3477), b(kDisplayLayout3486);
    for (DisplayRig* r : {&a, &b}) {
        // The display registers its own memory for change tracking.
        const uint64_t before = r->bus.observed_writes();
        r->bus.mem_write8(0xC200, 0x41);
        r->bus.mem_write8(0x20117, 3);
        CHECK(r->bus.observed_writes() == before + 2);

        // 80 x 24+1 rows of 12-dot cells, main window at C200; one cell
        // with a glyph whose first line is a single dot.
        const uint8_t on = r == &a ? 0x10 : 0x40;
        for (auto rv : std::initializer_list<std::pair<uint32_t, uint8_t>>{
                 {0x0F, 20}, {0x14, 25}, {0x15, 24}, {0x2A, 0x01}, {0x30, 0x00}, {0x31, 0x42},
                 {0x34, 0x00}, {0x35, 0x6E}, {0x28, on}})
            r->bus.mem_write8(0x20100 + rv.first, rv.second);
        r->bus.poke8(0x18000 + 0x41 * 32u, 0x20);   // dot 2 of line 0
        r->bus.poke8(0xC200 + 1, 0x00);   // a plain cell (20 is non-display on the 3486 family)
    }
    auto lit = [](DisplayRig& r) {
        Framebuffer fb;
        r.disp.render(fb);
        int n = 0;
        for (int x = 0; x < fb.width / 80; ++x)
            if ((fb.pixels[size_t(x)] & 0x00FFFFFFu) != 0) ++n;
        return std::make_pair(fb.width, n);
    };
    // 3477: a pixel per dot. 3486 family: two pixels per dot and half a dot
    // more for every lit dot.
    CHECK(lit(a) == std::make_pair(960, 1));
    CHECK(lit(b) == std::make_pair(1920, 3));
}

TEST_CASE("3486 display: the signature selector is 2012F bits 5-4") {
    DisplayRig a(kDisplayLayout3477), b(kDisplayLayout3486);
    REQUIRE(a.disp.vdo_shim());
    // The 3477 reads bits 6-5: selector 0, which never measures 0.
    CHECK(a.measure() == a.disp.vdo_shim_signature(0));
    CHECK(b.measure() == b.disp.vdo_shim_signature(1));
    CHECK(a.disp.vdo_shim_signature(0) != 0);
}

TEST_CASE("20025 bit 6 sends two frames only on a three-channel engine") {
    for (unsigned channels : {2u, 3u}) {
        Bus bus{20};
        bus.map_ram("ram", 0, 0x10000);
        TwinaxEngine eng(bus, channels, kClock, channels == 3 ? kLinkLayout3486 : kLinkLayout3477);
        uint8_t req = 0;
        eng.set_raise([&req](unsigned, uint8_t bits) { req |= bits; });
        eng.set_pending([&req](unsigned) { return uint8_t(req & 0x7F); });
        eng.reset();
        TwinaxRegisters regs(eng);
        regs.reset();
        // Channel 0, queue program that stores the message's frames.
        uint32_t a = 0x0080;
        const std::initializer_list<uint8_t> queue_program = {0x55, 0x22, 0x56, 0x20, 0x57, 0x21, 0x5F, 0x23, 0x82,
                                                              0xA2, 0x40, 0x36, 0x41, 0x37, 0x42, 0x35, 0x84};
        for (uint8_t b : queue_program) bus.poke8(a++, b);
        bus.poke8(0x0185, 0x80);
        bus.poke8(0x0186, 0xFF);
        bus.poke8(0x0187, 0x3F);
        regs.twinax_write(0x08, 0x02);
        regs.twinax_write(0x24, 0x02);
        regs.twinax_write(0x25, 0x42);
        regs.twinax_write(0x26, 0x0B);
        regs.twinax_write(0x27, 0xEE);
        regs.twinax_write(0x09, 0xFB);
        regs.twinax_write(0x15, 0x48);
        // One frame queued on the 3477's engine, two with a third channel.
        CHECK(bus.peek8(0x0185) == (channels > 2 ? 0x7E : 0x7F));
    }
}

TEST_CASE("3486 and 3487 models: one board, straps from the display subtype") {
    const Model* m3486 = find_model("3486");
    const Model* m3487 = find_model("ibm-3487");
    REQUIRE(m3486);
    REQUIRE(m3487);
    CHECK(m3487->info().name == "3487");
    CHECK(m3487->info().status == ModelStatus::Bringup);
    REQUIRE(m3487->info().roms.size() == 1);
    CHECK(m3487->info().roms[0].file == m3486->info().roms[0].file);
    CHECK(m3487->info().roms[0].sha256 == m3486->info().roms[0].sha256);

    // One monochrome unit whatever its tube; only the 3487 has a colour one.
    REQUIRE(m3486->info().variants.size() == 1);
    CHECK(m3486->default_variant().id == "3486-BA");
    CHECK(m3486->default_variant().straps == 0x42);
    CHECK(m3486->default_variant().phosphors.size() == 3);
    CHECK(m3486->variant_with_colour(true) == nullptr);
    REQUIRE(m3487->info().variants.size() == 2);
    CHECK(m3487->default_variant().id == "3487-HG");
    CHECK(m3487->default_variant().straps == 0x4A);
    REQUIRE(m3487->find_variant("hc"));
    CHECK(m3487->find_variant("3487-HC")->straps == 0x4C);
    CHECK(m3487->find_variant("HC")->colour);
    CHECK(m3487->find_variant("HC")->phosphors.empty());
    // Paper-white is a phosphor of the monochrome unit, not straps of its own.
    std::string variant, phosphor;
    select_legacy_display(*m3487, DisplaySubtype::PaperWhite, variant, phosphor);
    CHECK(variant == "3487-HG");
    CHECK(phosphor == "paperwhite");
    select_legacy_display(*m3487, DisplaySubtype::Color, variant, phosphor);
    CHECK(variant == "3487-HC");
    CHECK(phosphor.empty());
    select_legacy_display(*m3486, DisplaySubtype::Color, variant, phosphor);
    CHECK(variant == "3486-BA");   // no colour 3486: its only variant
}

TEST_CASE("interrupt controller state shown per board") {
    GateArrayInterrupts ic;
    CHECK(ic.describe().find("FF=") == std::string::npos);
    CHECK(ic.describe().find("0D=") == std::string::npos);
    CHECK(ic.describe().find("third channel") == std::string::npos);
    ic.set_three_channel_layout();
    ic.request2(0x08);
    ic.set_mask2(0x7F);
    const std::string t = ic.describe();
    CHECK(t.find("third channel: request 08 mask 7F") != std::string::npos);
    CHECK(t.find("0D=") != std::string::npos);
    CHECK(t.find("0F=") != std::string::npos);
}

namespace {
// One byte written the way the firmware does: device address, word
// address, data, stop.
void eeprom_write(I2cEeprom& e, uint8_t address, uint8_t value) {
    REQUIRE(e.i2c_select(0x50, false));
    e.i2c_write(address);
    e.i2c_write(value);
    e.i2c_stop();
}
} // namespace

TEST_CASE("3486 family display: colour guns in the extension byte, non-display in the attribute") {
    // One solid glyph in a 24x80 screen of 12-dot cells, picture on (20128
    // bit 6); cell = character, attribute, extension.
    auto rig = [](DisplayRig& r) {
        auto reg = [&](uint32_t n, uint8_t v) { r.bus.mem_write8(0x20100 + n, v); };
        reg(0x0F, 20);
        reg(0x14, 25);
        reg(0x15, 24);
        reg(0x2A, 0x01);
        reg(0x31, 0x42);
        reg(0x35, 0x6E);
        reg(0x28, 0x40);
        for (int line = 0; line < 21; ++line) r.bus.poke8(0x18000 + 0xC1 * 32 + uint32_t(line), 0xFF);
    };
    auto cell = [](DisplayRig& r, int col, uint8_t attr, uint8_t ext) {
        r.bus.poke8(0xC200 + uint32_t(col) * 3, 0xC1);
        r.bus.poke8(0xC200 + uint32_t(col) * 3 + 1, attr);
        r.bus.poke8(0xC200 + uint32_t(col) * 3 + 2, ext);
    };
    auto at = [](const Framebuffer& fb, int col) { return fb.pixels[size_t(col * 24 + 4)]; };
    auto red_of = [](uint32_t c) { return (c >> 16) & 0xFF; };
    auto green_of = [](uint32_t c) { return (c >> 8) & 0xFF; };
    auto blue_of = [](uint32_t c) { return c & 0xFF; };

    DisplayRig c(kDisplayLayout3486, true);
    rig(c);
    cell(c, 0, 0x00, 0x04);   // green
    cell(c, 1, 0x00, 0x02);   // blue
    cell(c, 2, 0x00, 0x08);   // red
    cell(c, 3, 0x00, 0x0C);   // yellow
    cell(c, 4, 0x00, 0x0E);   // white
    cell(c, 5, 0x20, 0x04);   // non-display
    Framebuffer fb;
    c.disp.render(fb);
    REQUIRE(fb.width == 80 * 24);
    CHECK(green_of(at(fb, 0)) > red_of(at(fb, 0)));
    CHECK(blue_of(at(fb, 1)) > red_of(at(fb, 1)));
    CHECK(red_of(at(fb, 2)) > green_of(at(fb, 2)));
    CHECK(blue_of(at(fb, 3)) < green_of(at(fb, 3)));
    CHECK(red_of(at(fb, 3)) > 0x80);
    CHECK(blue_of(at(fb, 4)) > 0x80);
    CHECK(red_of(at(fb, 4)) > 0x80);
    CHECK(at(fb, 5) == 0xFF000000u);

    // The monochrome unit hides the same way.
    DisplayRig m(kDisplayLayout3486);
    rig(m);
    cell(m, 0, 0x00, 0x00);
    cell(m, 1, 0x20, 0x00);
    m.disp.render(fb);
    CHECK(at(fb, 0) != 0xFF000000u);
    CHECK(at(fb, 1) == 0xFF000000u);
}

TEST_CASE("3486 colour unit: the alignment stand-in marks EEPROM byte 30 once the firmware initialises it") {
    std::string err;
    // Without the stand-in the firmware's own values stay as written.
    {
        I2cEeprom plain("eeprom");
        REQUIRE(plain.open("", err));
        eeprom_write(plain, 0x30, 0x55);
        eeprom_write(plain, 0x00, 0xAA);
        CHECK(plain.byte(0x30) == 0x55);
    }
    // A blank EEPROM: nothing until the "initialised" mark is written, then
    // byte 30 follows; the other bytes are left alone.
    I2cEeprom e("eeprom");
    REQUIRE(e.open("", err));
    int cycles = 0;
    install_monitor_alignment_shim(e, [&cycles] { ++cycles; });
    CHECK(e.byte(0x30) != 0xAA);
    eeprom_write(e, 0x2F, 0x12);
    eeprom_write(e, 0x30, 0x55);
    eeprom_write(e, 0x31, 0x34);
    CHECK(e.byte(0x30) == 0x55);
    eeprom_write(e, 0x00, 0x00);    // not the mark
    CHECK(e.byte(0x30) == 0x55);
    CHECK(cycles == 0);
    eeprom_write(e, 0x00, 0xAA);
    CHECK(e.byte(0x30) == 0xAA);
    CHECK(cycles == 1);             // the firmware reads it at the next power-on
    eeprom_write(e, 0x00, 0xAA);    // already marked: no further power cycle
    CHECK(cycles == 1);
    CHECK(e.byte(0x2F) == 0x12);
    CHECK(e.byte(0x31) == 0x34);
    // An image initialised earlier (by a run without the stand-in) is
    // marked when the stand-in is installed.
    I2cEeprom old("eeprom");
    REQUIRE(old.open("", err));
    eeprom_write(old, 0x30, 0x55);
    eeprom_write(old, 0x00, 0xAA);
    install_monitor_alignment_shim(old, [&cycles] { ++cycles; });
    CHECK(old.byte(0x30) == 0xAA);
    CHECK(cycles == 1);             // marked before its power-on: none needed
}
