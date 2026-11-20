// Twinax link engine: programs, descriptors, data movers, events and the
// 3477 loopback test registers. The engine programs below are written for
// these tests from the opcode set documented in docs/bringup-3477.md.
#include "bus/bus.hpp"
#include "devices/ibm3477/twinax_engine.hpp"
#include "host/twinax_link.hpp"

#include <doctest/doctest.h>

#include <initializer_list>
#include <memory>

using namespace tn34xx;

namespace {

constexpr uint64_t kClock = 8000000;

// Test programs (one per class: queue, write, poll, read).
const std::initializer_list<uint8_t> kQueue = {0x55, 0x22, 0x56, 0x20, 0x57, 0x21, 0x5F, 0x23, 0x82,
                                               0xA2, 0x40, 0x36, 0x41, 0x37, 0x42, 0x35, 0x84};
const std::initializer_list<uint8_t> kWrite = {0x54, 0x88, 0x5F, 0x34, 0x50, 0x20, 0x51, 0x21, 0x52, 0x22,
                                               0x53, 0x23, 0xA2, 0x40, 0x30, 0x41, 0x31, 0x90, 0x84};
const std::initializer_list<uint8_t> kRead = {0x54, 0x88, 0x5F, 0x34, 0x50, 0x20, 0x51, 0x21, 0x52, 0x22,
                                              0x53, 0x23, 0xC0, 0xA1, 0x40, 0x30, 0x41, 0x31, 0x90, 0x84};

struct Rig {
    Bus bus{20};
    std::unique_ptr<TwinaxEngine> eng;
    uint16_t req = 0;   // request bits, channel n in byte n

    explicit Rig(unsigned channels = 2) {
        bus.map_ram("ram", 0, 0x10000);
        eng = std::make_unique<TwinaxEngine>(bus, channels, kClock);
        eng->set_raise([this](unsigned ch, uint8_t bits) { req = uint16_t(req | (bits << (8 * ch))); });
        eng->set_pending([this](unsigned ch) { return uint8_t((req >> (8 * ch)) & 0x7F); });
        eng->reset();
        // Class table: Read Activate 00 -> C0, Write Activate 01 -> 40,
        // Poll 10 -> 80, everything else 00 (queue).
        bus.poke8(0x60 + 0x00, 0xC0);
        bus.poke8(0x60 + 0x01, 0x40);
        bus.poke8(0x60 + 0x10, 0x80);
        for (unsigned ch = 0; ch < channels && ch < 2; ++ch) {
            load(ch, 0, kQueue);
            load(ch, 1, kWrite);
            load(ch, 2, {0x58, 0x24, 0x5A, 0x25, 0x5C, 0x26, 0x5D, uint8_t(0x27 + ch), 0xC0, 0x5F, 0x3B, 0x84});
            load(ch, 3, kRead);
            eng->set_station_address(ch, uint8_t(ch ? 5 : 2));
            eng->set_channel_enabled(ch, true);
            desc(ch, 0x0C, 0x0E);   // power-on exception
            desc(ch, 0x0A, 0x02);
        }
        eng->set_running(true);
    }
    void load(unsigned ch, unsigned prog, std::initializer_list<uint8_t> bytes) {
        uint32_t a = 0x80 + 0x80 * ch + 32 * prog;
        for (uint8_t b : bytes) bus.poke8(a++, b);
    }
    void desc(unsigned ch, unsigned off, uint8_t v) { bus.poke8(0x180 + 16 * ch + off, v); }
    uint8_t desc(unsigned ch, unsigned off) const { return bus.peek8(0x180 + 16 * ch + off); }
    uint16_t desc16(unsigned ch, unsigned off) const {
        return uint16_t(desc(ch, off) | (desc(ch, off + 1) << 8));
    }
    void desc16(unsigned ch, unsigned off, uint16_t v) {
        desc(ch, off, uint8_t(v));
        desc(ch, off + 1, uint8_t(v >> 8));
    }
    // Send a message as the controller and return the station's response.
    TwinaxMessage send(TwinaxMessage msg) {
        auto host = std::make_unique<ScriptTwinaxHost>();
        ScriptTwinaxHost* h = host.get();
        eng->set_host([h]() -> TwinaxHost* { return h; });
        h->queue(std::move(msg));
        for (int i = 0; i < 2000; ++i) eng->tick(80);   // 20 ms of line time
        auto got = h->take_received();
        eng->set_host(nullptr);
        return got.empty() ? TwinaxMessage{} : got.front();
    }
};

TwinaxMessage poll(uint8_t addr, uint8_t cmd = 0x30) { return {TwinaxFrame{cmd, addr}}; }

} // namespace

TEST_CASE("twinax engine: poll is answered from the descriptor") {
    Rig r;
    // Before the two-frame response is enabled: one frame, address 7.
    TwinaxMessage resp = r.send(poll(2));
    REQUIRE(resp.size() == 1);
    CHECK(resp[0].address == 7);
    CHECK((resp[0].data & 0x1E) == 0x0E);   // power-on exception
    CHECK((resp[0].data & 0x80) == 0);      // not busy
    CHECK((r.req & 0x01) != 0);             // event: response sent
    CHECK(r.desc(0, 0x0B) == 0x00);         // program cleared the polled flag
    r.req = 0;

    // Two-frame response: status with the station address, then byte 2.
    r.desc(0, 0x0A, 0x06);
    r.desc(0, 0x0C, 0x00);
    r.desc(0, 0x0D, 0x45);
    resp = r.send(poll(2));
    REQUIRE(resp.size() == 2);
    CHECK(resp[0].address == 2);
    CHECK((resp[0].data & 0x9E) == 0);
    CHECK(resp[1].data == 0x45);
    CHECK(resp[1].address == 7);

    // Messages for other addresses get no response.
    CHECK(r.send(poll(3)).empty());
}

TEST_CASE("twinax engine: activate write and read move data by DMA") {
    Rig r;
    // Write Activate into 0x3000, room for 4 bytes.
    r.desc16(0, 0, 0x3000);
    r.desc16(0, 2, 4);
    r.desc(0, 4, 1);
    CHECK(r.send({{0x01, 2}, {0xC1, 2}, {0xC2, 2}, {0xC3, 7}}).empty());
    CHECK(r.bus.peek8(0x3000) == 0xC1);
    CHECK(r.bus.peek8(0x3002) == 0xC3);
    CHECK(r.desc16(0, 0) == 0x3003);   // address after the last byte
    CHECK(r.desc16(0, 2) == 4);        // the program writes back only the address
    CHECK(r.desc(0, 4) == 0);          // disarmed
    CHECK((r.req & 0x04) != 0);        // activate done
    CHECK(r.eng->busy(0));
    CHECK(r.eng->read_control(0) == 0x80);
    r.eng->write_control(0, 0x80);
    CHECK_FALSE(r.eng->busy(0));
    r.req = 0;

    // Read Activate: three bytes out, the last one addressed 7.
    r.bus.poke8(0x3100, 0xC2);
    r.bus.poke8(0x3101, 0x24);
    r.bus.poke8(0x3102, 0x50);
    r.desc16(0, 0, 0x3100);
    r.desc16(0, 2, 3);
    r.desc(0, 4, 2);
    TwinaxMessage resp = r.send({{0x00, 2}});
    REQUIRE(resp.size() == 3);
    CHECK(resp[0].data == 0xC2);
    CHECK(resp[0].address == 2);
    CHECK(resp[2].data == 0x50);
    CHECK(resp[2].address == 7);
    CHECK(r.desc16(0, 0) == 0x3103);
    CHECK((r.req & 0x04) != 0);

    // An activate with nothing armed is invalid.
    r.req = 0;
    CHECK(r.send({{0x01, 2}, {0x11, 7}}).empty());
    CHECK((r.req & 0x20) != 0);
    CHECK(r.bus.peek8(0x3103) == 0);
}

TEST_CASE("twinax engine: other commands are queued for the firmware") {
    Rig r;
    r.desc16(0, 6, 0x1F10);
    r.desc(0, 5, 0x10);
    CHECK(r.send({{0x13, 2}, {0x00, 7}}).empty());   // Set Mode, 2 bytes
    CHECK(r.bus.peek8(0x1F10) == 0x13);
    CHECK(r.bus.peek8(0x1F11) == 0x00);
    CHECK(r.desc16(0, 6) == 0x1F12);
    CHECK(r.desc(0, 5) == 0x0E);
    CHECK((r.req & 0x0A) == 0x0A);   // queued + end of message
    // Busy until the firmware has dealt with the queue.
    TwinaxMessage resp = r.send(poll(2));
    REQUIRE(resp.size() == 1);
    CHECK((resp[0].data & 0x80) != 0);
    r.req = 0;
    r.eng->write_control(0, 0x80);
    resp = r.send(poll(2));
    CHECK((resp[0].data & 0x80) == 0);

    // Overrun: more frames than room.
    r.req = 0;
    r.desc(0, 5, 2);
    r.send({{0x11, 2}, {0x03, 2}, {0xC1, 2}, {0xC2, 7}});
    CHECK((r.req & 0x10) != 0);
    CHECK(r.desc(0, 5) == 0);
}

TEST_CASE("twinax engine: the two channels are independent") {
    Rig r;
    r.desc16(0, 6, 0x1F10);
    r.desc(0, 5, 0x10);
    r.desc16(1, 6, 0x3910);
    r.desc(1, 5, 0x10);
    r.send({{0x12, 5}});   // Clear to channel B
    CHECK(r.bus.peek8(0x3910) == 0x12);
    CHECK(r.desc16(1, 6) == 0x3911);
    CHECK(r.desc16(0, 6) == 0x1F10);
    CHECK((r.req & 0x00FF) == 0);
    CHECK((r.req & 0x0200) != 0);
    CHECK(r.eng->busy(1));
    CHECK_FALSE(r.eng->busy(0));

    // Each channel answers with its own byte 2 register.
    r.req = 0;
    r.desc(0, 0x0A, 0x06);
    r.desc(1, 0x0A, 0x0A);
    r.desc(0, 0x0D, 0x11);
    r.desc(1, 0x0D, 0x22);
    CHECK(r.send(poll(2))[1].data == 0x11);
    CHECK(r.send(poll(5))[1].data == 0x22);

    // A disabled channel does not answer.
    r.eng->set_channel_enabled(1, false);
    CHECK(r.send(poll(5)).empty());

    // The channel count is a parameter.
    Rig four(4);
    CHECK(four.eng->channels() == 4);
}

TEST_CASE("twinax engine: toggle, response level and busy from pending events") {
    Rig r;
    r.desc(0, 0x0C, 0x00);
    const uint8_t a = r.send(poll(2))[0].data;
    // The previous response's event is still pending: busy.
    const uint8_t b = r.send(poll(2))[0].data;
    CHECK(((a ^ b) & 0x20) != 0);
    CHECK((a & 0x80) == 0);
    CHECK((b & 0x80) != 0);
    r.req = 0;
    // A new keyboard byte flips the response level on the next poll.
    r.eng->write_control(0, 0x10);
    const uint8_t c = r.send(poll(2))[0].data;
    CHECK(((b ^ c) & 0x01) != 0);
    CHECK(((b ^ c) & 0x20) != 0);
    r.req = 0;
    const uint8_t d = r.send(poll(2))[0].data;
    CHECK(((c ^ d) & 0x01) == 0);
}

TEST_CASE("twinax engine: host line timing follows the CPU clock") {
    Rig r;
    ScriptTwinaxHost host;
    r.eng->set_host([&host]() -> TwinaxHost* { return &host; });
    host.queue(poll(2));
    host.queue(poll(6));
    // A poll takes its transmit time, the turnaround and the response time.
    const uint64_t need = r.eng->message_cycles(1) * 2 + r.eng->turnaround_cycles();
    uint64_t t = 0;
    while (t < need - 8) {
        r.eng->tick(8);
        t += 8;
    }
    CHECK(host.take_received().empty());
    for (int i = 0; i < 400; ++i) r.eng->tick(8);
    CHECK(host.take_received().size() == 1);
    for (int i = 0; i < 2000; ++i) r.eng->tick(8);
    CHECK(host.describe().find("1 without response") != std::string::npos);
    // A held engine takes nothing from the line.
    r.eng->set_running(false);
    host.queue(poll(2));
    for (int i = 0; i < 2000; ++i) r.eng->tick(8);
    CHECK(host.take_received().empty());
}

TEST_CASE("twinax engine: 3477 loopback test registers") {
    Rig r;
    Ibm3477TwinaxRegisters regs(*r.eng);
    regs.reset();
    r.eng->set_channel_enabled(1, false);
    // Station addresses A = 2, B = 5; channel A enabled, engine running.
    regs.twinax_write(0x08, 0x2A);
    CHECK(r.eng->station_address(0) == 2);
    CHECK(r.eng->station_address(1) == 5);
    regs.twinax_write(0x09, 0x7B);
    CHECK_FALSE(r.eng->running());
    CHECK(r.eng->channel_enabled(0));
    CHECK_FALSE(r.eng->channel_enabled(1));

    // A two-frame message from the frame generator is queued.
    r.desc16(0, 6, 0x3FFF);
    r.desc(0, 5, 0x80);
    regs.twinax_write(0x24, 0x02);
    regs.twinax_write(0x25, 0x06);
    regs.twinax_write(0x26, 0x14);
    regs.twinax_write(0x27, 0xEE);
    regs.twinax_write(0x28, 0xEE);
    regs.twinax_write(0x09, 0xFB);
    regs.twinax_write(0x15, 0x48);   // loopback, path select: transmit
    CHECK(r.eng->wrap());
    CHECK(r.desc(0, 5) == 0x7E);
    CHECK(r.desc16(0, 6) == 0x4001);
    CHECK(r.bus.peek8(0x4000) == 0xEE);

    // Poll: the response's status byte lands in the receive latch.
    r.req = 0;
    regs.twinax_write(0x00, 0x80);
    r.desc(0, 0x0A, 0x02);
    r.desc(0, 0x0C, 0x00);
    regs.twinax_write(0x25, 0x02);
    regs.twinax_write(0x26, 0x10);
    regs.twinax_write(0x15, 0x48);
    CHECK((regs.twinax_read(0x2B) & 0x80) == 0);
    regs.twinax_write(0x15, 0x48);
    CHECK((regs.twinax_read(0x2B) & 0x80) != 0);   // previous event pending

    // Forced parity error: no response, LPE latched, then shown and reset.
    regs.twinax_write(0x15, 0x58);
    CHECK(r.eng->line_parity_error(0));
    CHECK((r.req & 0x40) != 0);
    regs.twinax_write(0x15, 0x48);
    CHECK((regs.twinax_read(0x2B) & 0x40) != 0);
    regs.twinax_write(0x26, 0x50);
    regs.twinax_write(0x15, 0x48);
    CHECK((regs.twinax_read(0x2B) & 0x40) == 0);

    // Transmit DMA loops a RAM buffer back; the latch keeps the last byte.
    for (unsigned i = 0; i < 8; ++i) r.bus.poke8(0x3FFC + i, uint8_t(0x61 + i));
    r.bus.poke8(0x60 + 0x01, 0x00);   // 0x61 & 1F = 01: make it a queued command
    r.desc16(0, 6, 0x2000);
    r.desc(0, 5, 0x10);
    regs.twinax_write(0x20, 0xFC);
    regs.twinax_write(0x21, 0x3F);
    regs.twinax_write(0x22, 0x08);
    regs.twinax_write(0x23, 0x00);
    regs.twinax_write(0x25, 0x20);
    regs.twinax_write(0x15, 0xC8);
    CHECK(regs.twinax_read(0x2B) == 0x68);
    CHECK(r.desc16(0, 6) == 0x2008);

    // Normal operation: no loopback, the frame generator is quiet.
    regs.twinax_write(0x15, 0x0C);
    CHECK_FALSE(r.eng->wrap());
    r.desc16(0, 6, 0x2100);
    regs.twinax_write(0x15, 0x0C);
    CHECK(r.desc16(0, 6) == 0x2100);
}
