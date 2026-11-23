// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware the 3486 family adds to the 3477 design: the second serial port,
// the third Twinax station channel and the display's sync polarity bit.
#include "bus/bus.hpp"
#include "devices/ibm3486/alignment_shim.hpp"
#include "devices/ibm3486/aux_port.hpp"
#include "devices/ibm3477/i2c.hpp"
#include "devices/ibm3477/display.hpp"
#include "devices/ibm3477/keyboard_if.hpp"
#include "devices/ibm3477/system.hpp"
#include "devices/ibm3477/twinax_engine.hpp"
#include "input/ps2_mouse.hpp"
#include "models/model.hpp"
#include "util/strings.hpp"

#include <doctest/doctest.h>

#include <initializer_list>
#include <memory>
#include <string>

using namespace em34xx;
using Src = Ibm3477Interrupts::Source;

namespace {
constexpr uint64_t kClock = 7500000;

std::unique_ptr<Ibm3477System> make_system() {
    auto k = std::make_unique<Ibm3477KeyboardInterface>(kClock, KeyboardType::Terminal122, 0xBF, 0xBF);
    auto sys = std::make_unique<Ibm3477System>(kClock, uint8_t(0x42), std::move(k));
    sys->reset();
    return sys;
}
} // namespace

TEST_CASE("3486 second serial port: manual-clock wrap and its interrupt") {
    auto sys = make_system();
    Ibm3477Interrupts& ic = sys->interrupts();
    ic.set_three_channel_layout();
    Ibm3486AuxPort aux;
    aux.connect(&ic);
    aux.reset();
    CHECK((aux.read(1) & 0x80) == 0);   // layout bit

    for (uint8_t byte : {uint8_t(0xAB), uint8_t(0x54)}) {
        aux.write(1, 0x01);
        aux.write(2, 0xFF);
        aux.write(0, byte);
        for (int clock = 0; clock < 11; ++clock) {
            CHECK((aux.read(1) & 0x04) == 0);
            aux.write(1, 0x01);
            aux.write(1, 0x03);
        }
        CHECK((aux.read(1) & 0x04) != 0);   // byte received
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
        CHECK((aux.read(1) & 0x04) == 0);
        sys->mmio_write8(0x07, 0x00);
    }
    // ...and 1 for a byte with an even number of ones.
    aux.write(0, 0x03);
    for (int clock = 0; clock < 11; ++clock) {
        aux.write(1, 0x01);
        aux.write(1, 0x03);
    }
    CHECK((aux.read(1) & 0x05) == 0x05);

    // A byte sent with the line released goes nowhere: no device.
    aux.write(2, 0xFF);
    aux.write(0, 0xFF);
    aux.write(1, 0x02);
    CHECK((aux.read(1) & 0x04) == 0);
    CHECK_FALSE(ic.line(Src::Aux));
}

TEST_CASE("3486 third Twinax channel: registers, programs at 0200, descriptor at 01A0") {
    Bus bus{20};
    bus.map_ram("ram", 0, 0x10000);
    TwinaxEngine eng(bus, 3, kClock);
    auto sys = make_system();
    sys->set_third_channel(true);
    Ibm3477Interrupts& ic = sys->interrupts();
    eng.set_raise([&ic](unsigned ch, uint8_t bits) {
        if (ch == 2) ic.request2(bits);
        else ic.request(uint16_t(bits << (8 * ch)));
    });
    eng.set_pending([&ic](unsigned ch) {
        return uint8_t((ch == 2 ? ic.requests2() : uint8_t(ic.requests() >> (8 * ch))) & 0x7F);
    });
    eng.reset();
    Ibm3477TwinaxRegisters regs(eng);
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
    auto sys = make_system();
    for (uint32_t off : {0x0Bu, 0x0Du, 0x0Fu, 0x1Au, 0x1Bu}) {
        sys->mmio_write8(off, 0xA5);
        CHECK(sys->mmio_read8(off) == 0xA5);
    }
}

TEST_CASE("3486 display: retrace length and the sync polarity bit") {
    Bus bus{20};
    TerminalOptions opts;
    Ibm3477Display disp(bus, opts);
    bus.map_ram("ram", 0, 0x20000);
    bus.map_mmio("display", 0x20100, 0x100, &disp);
    disp.set_clock_hz(kClock);
    const uint64_t frame = disp.cycles_per_frame();

    auto retrace_cycles = [&]() {
        while (!disp.in_retrace()) disp.tick(1);
        uint64_t n = 0;
        while (disp.in_retrace()) {
            disp.tick(1);
            ++n;
        }
        return n;
    };
    retrace_cycles();   // align to a whole retrace
    CHECK(retrace_cycles() == frame / 90);
    disp.set_retrace_divisor(140);
    retrace_cycles();
    CHECK(retrace_cycles() == frame / 140);

    // 20128 bit 3 inverts the flag only when the board has that bit.
    while (!disp.in_retrace()) disp.tick(1);
    bus.mem_write8(0x20128, 0x18);
    CHECK((bus.mem_read8(0x2012F) & 0x08) != 0);
    disp.set_sync_polarity_bit(true);
    CHECK((bus.mem_read8(0x2012F) & 0x08) == 0);
    bus.mem_write8(0x20128, 0x10);
    CHECK((bus.mem_read8(0x2012F) & 0x08) != 0);

    // Picture on: 20128 bit 4 on the 3477, bit 6 on the 3486 family (whose
    // colour tube never sets bit 4).
    disp.set_sync_polarity_bit(false);
    bus.mem_write8(0x20114, 25);
    bus.mem_write8(0x20115, 24);
    bus.mem_write8(0x20128, 0x10);
    CHECK(disp.enabled());
    bus.mem_write8(0x20128, 0x40);
    CHECK_FALSE(disp.enabled());
    disp.set_video_enable_bit6(true);
    CHECK(disp.enabled());
    bus.mem_write8(0x20128, 0x18);
    CHECK_FALSE(disp.enabled());
}

TEST_CASE("3486 display: the signature selector is 2012F bits 5-4") {
    Bus bus{20};
    TerminalOptions opts;
    Ibm3477Display disp(bus, opts);
    bus.map_ram("ram", 0, 0x20000);
    bus.map_mmio("display", 0x20100, 0x100, &disp);
    disp.set_clock_hz(kClock);
    REQUIRE(disp.vdo_shim());

    // Start a measurement with 2012F = 10 then 90 at the start of a retrace
    // and read the result one field later.
    auto measure = [&]() {
        while (disp.in_retrace()) disp.tick(16);
        while (!disp.in_retrace()) disp.tick(16);
        bus.mem_write8(0x2012F, 0x10);
        bus.mem_write8(0x2012F, 0x90);
        while (disp.in_retrace()) disp.tick(16);
        while (!disp.in_retrace()) disp.tick(16);
        const uint8_t v = uint8_t(bus.mem_read8(0x2012F) >> 4);
        bus.mem_write8(0x2012F, 0x00);
        return v;
    };
    // The 3477 reads bits 6-5: selector 0, which never measures 0.
    CHECK(measure() == disp.vdo_shim_signature(0));
    disp.set_signature_selector_shift(4);
    CHECK(measure() == disp.vdo_shim_signature(1));
    CHECK(disp.vdo_shim_signature(0) != 0);
}

TEST_CASE("20025 bit 6 sends two frames only on a three-channel engine") {
    for (unsigned channels : {2u, 3u}) {
        Bus bus{20};
        bus.map_ram("ram", 0, 0x10000);
        TwinaxEngine eng(bus, channels, kClock);
        uint8_t req = 0;
        eng.set_raise([&req](unsigned, uint8_t bits) { req |= bits; });
        eng.set_pending([&req](unsigned) { return uint8_t(req & 0x7F); });
        eng.reset();
        Ibm3477TwinaxRegisters regs(eng);
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

    // A tube change restarts the terminal whenever it changes the straps,
    // also between two monochrome tubes; green and amber share a code.
    TerminalOptions o;
    o.display = DisplaySubtype::Green;
    for (const Model* m : {m3486, m3487}) {
        CHECK(m->display_change_needs_power_cycle(o, DisplaySubtype::PaperWhite));
        CHECK(m->display_change_needs_power_cycle(o, DisplaySubtype::Color));
        CHECK_FALSE(m->display_change_needs_power_cycle(o, DisplaySubtype::Amber));
    }
    // Explicit straps are kept: only the colour/monochrome change restarts.
    o.straps = 0x4A;
    CHECK_FALSE(m3487->display_change_needs_power_cycle(o, DisplaySubtype::PaperWhite));
    CHECK(m3487->display_change_needs_power_cycle(o, DisplaySubtype::Color));
}

TEST_CASE("interrupt controller state shown per board") {
    Ibm3477Interrupts ic;
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

TEST_CASE("PS/2 mouse: reset, identification and movement packets") {
    Ps2Mouse m;
    m.power_on();
    auto take = [&m]() {
        std::string s;
        while (m.has_output()) s += format("%02X ", m.read_output());
        return s;
    };
    CHECK(take() == "AA 00 ");
    m.host_write(0xFF);
    CHECK(take() == "FA AA 00 ");
    m.host_write(0xF2);
    CHECK(take() == "FA 00 ");
    m.host_write(0xF3);
    m.host_write(0x28);
    m.host_write(0xE8);
    m.host_write(0x03);
    CHECK(take() == "FA FA FA FA ");
    m.host_write(0xE9);
    CHECK(take() == "FA 00 03 28 ");
    m.host_write(0x00);              // not a command
    CHECK(take() == "FE ");

    // Nothing is reported until the host enables it.
    m.move(5, 5, 0);
    CHECK_FALSE(m.has_output());
    m.host_write(0xF4);
    CHECK(take() == "FA ");
    m.move(3, -2, 0);                // right and away from the user (up)
    CHECK(take() == "08 03 02 ");
    m.move(-3, 2, Ps2Mouse::kLeft);  // left and down, button held
    CHECK(take() == "39 FD FE ");
    m.move(0, 0, 0);                 // release
    CHECK(take() == "08 00 00 ");
    m.move(0, 0, 0);                 // no change, no packet
    CHECK_FALSE(m.has_output());
}

TEST_CASE("3486 second serial port with a mouse: bytes both ways, a held line abandons a byte") {
    auto sys = make_system();
    Ibm3477Interrupts& ic = sys->interrupts();
    ic.set_three_channel_layout();
    Ibm3486AuxPort aux(kClock);
    aux.connect(&ic);
    aux.attach_mouse(true);
    aux.reset();
    sys->mmio_write8(0x07, 0x02);
    auto run = [&aux](unsigned us) { aux.tick(kClock / 1000000 * us); };
    auto receive = [&]() {
        for (int i = 0; i < 50 && !(aux.read(1) & 0x04); ++i) run(100);
        REQUIRE((aux.read(1) & 0x04) != 0);
        CHECK(ic.acknowledge() == 0x0E);
        const uint8_t b = aux.read(0);
        aux.write(1, 0x03);
        aux.write(2, 0x00);
        aux.write(1, 0x02);
        return b;
    };
    // Power-on report once the line is released.
    aux.write(1, 0x02);
    CHECK(receive() == 0xAA);
    CHECK(receive() == 0x00);

    // A byte the host starts and then takes back never arrives.
    aux.write(1, 0x03);
    aux.write(0, 0x00);
    aux.write(1, 0x02);
    CHECK((aux.read(1) & 0x02) != 0);   // busy
    aux.write(1, 0x03);
    CHECK((aux.read(1) & 0x02) == 0);
    run(3000);
    CHECK_FALSE(aux.mouse().has_output());

    // A command gets through: transmit-complete interrupt, then the answer.
    aux.write(2, 0x00);
    aux.write(0, 0xF4);
    aux.write(1, 0x02);
    run(1100);
    CHECK(ic.line(Src::Aux));           // end of transmission
    CHECK((aux.read(1) & 0x04) == 0);
    aux.write(1, 0x03);
    aux.write(2, 0x00);
    aux.write(1, 0x02);
    CHECK(receive() == 0xFA);
    aux.pointer_event(2, 0, 0);
    CHECK(receive() == 0x08);
    CHECK(receive() == 0x02);
    CHECK(receive() == 0x00);
}
