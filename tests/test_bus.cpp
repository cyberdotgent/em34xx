// SPDX-License-Identifier: GPL-3.0-or-later
#include "bus/bus.hpp"

#include <doctest/doctest.h>

using namespace em34xx;

TEST_CASE("RAM, ROM and open bus") {
    Bus bus(20);
    bus.map_ram("ram", 0, 0x1000);
    bus.map_rom("rom", 0xF0000, std::vector<uint8_t>(0x10000, 0xC3));
    bus.set_policy(UnmappedPolicy::Ignore);

    bus.mem_write8(0x10, 0x42);
    CHECK(bus.mem_read8(0x10) == 0x42);
    CHECK(bus.mem_read8(0xF1234) == 0xC3);
    bus.mem_write8(0xF1234, 0x00);           // ROM is not writable
    CHECK(bus.mem_read8(0xF1234) == 0xC3);
    CHECK(bus.mem_read8(0x50000) == 0xFF);   // open bus
    CHECK(bus.mem_read8(0x100010) == 0x42);  // 20-bit wrap
}

TEST_CASE("stop policy stops once per distinct access") {
    Bus bus(20);
    bus.map_ram("ram", 0, 0x1000);
    bus.mem_read8(0x80000);
    REQUIRE(bus.stop_pending());
    CHECK(bus.stop_info().cause == BusStop::Cause::Unmapped);
    CHECK(bus.stop_info().address == 0x80000);
    bus.clear_stop();
    bus.mem_read8(0x80000);
    CHECK_FALSE(bus.stop_pending());
    CHECK(bus.unmapped_log().size() == 1);
    CHECK(bus.unmapped_log().begin()->second.count == 2);
    bus.mem_write8(0x80000, 1);   // a write is a distinct access
    CHECK(bus.stop_pending());
}

TEST_CASE("log policy never stops") {
    Bus bus(20);
    bus.set_policy(UnmappedPolicy::Log);
    bus.mem_write8(0x1234, 5);
    bus.io_write8(0x60, 1);
    CHECK_FALSE(bus.stop_pending());
    CHECK(bus.unmapped_log().size() == 2);
}

TEST_CASE("watchpoints") {
    Bus bus(20);
    bus.map_ram("ram", 0, 0x1000);
    const int id = bus.add_watchpoint(0x100, 2, AccessKind::Write);
    bus.mem_read8(0x100);
    CHECK_FALSE(bus.stop_pending());
    bus.mem_write8(0x101, 7);
    REQUIRE(bus.stop_pending());
    CHECK(bus.stop_info().cause == BusStop::Cause::Watchpoint);
    CHECK(bus.stop_info().watch_id == id);
    CHECK(bus.stop_info().address == 0x101);
    CHECK(bus.remove_watchpoint(id));
}

namespace {

// Records each access with its width: the word path must reach a device
// as one word, everything else as bytes.
struct WidthDevice : Device {
    WidthDevice() : Device("width") {}
    std::vector<std::string> log;
    uint8_t mmio_read8(uint32_t off) override { log.push_back("r8 " + std::to_string(off)); return uint8_t(0x10 + off); }
    void mmio_write8(uint32_t off, uint8_t v) override { log.push_back("w8 " + std::to_string(off) + "=" + std::to_string(v)); }
    uint16_t mmio_read16(uint32_t off) override { log.push_back("r16 " + std::to_string(off)); return 0xBEEF; }
    void mmio_write16(uint32_t off, uint16_t v) override { log.push_back("w16 " + std::to_string(off) + "=" + std::to_string(v)); }
    uint16_t io_read16(uint16_t port) override { log.push_back("in16 " + std::to_string(port)); return 0x1234; }
    void io_write16(uint16_t port, uint16_t v) override { log.push_back("out16 " + std::to_string(port) + "=" + std::to_string(v)); }
};

} // namespace

TEST_CASE("word cycles reach a device as one word access") {
    Bus bus(20);
    WidthDevice dev;
    bus.map_ram("ram", 0, 0x1000);
    bus.map_mmio("dev", 0x58000, 0x100, &dev);
    bus.map_io("dev", 0xFF00, 0x100, &dev);

    CHECK(bus.mem_read16(0x58004) == 0xBEEF);
    bus.mem_write16(0x5800A, 0x6001);
    CHECK(bus.io_read16(0xFF22) == 0x1234);
    bus.io_write16(0xFFFE, 0x18FF);
    CHECK(dev.log == std::vector<std::string>{"r16 4", "w16 10=24577", "in16 65314", "out16 65534=6399"});

    // RAM words are two bytes and keep byte watchpoints working.
    const int id = bus.add_watchpoint(0x101, 1, AccessKind::Write);
    bus.mem_write16(0x100, 0xA55A);
    CHECK(bus.mem_read16(0x100) == 0xA55A);
    REQUIRE(bus.stop_pending());
    CHECK(bus.stop_info().watch_id == id);
    CHECK(bus.stop_info().address == 0x101);
}

TEST_CASE("a word across a region boundary is two byte accesses") {
    Bus bus(20);
    WidthDevice dev;
    bus.set_policy(UnmappedPolicy::Ignore);
    bus.map_mmio("dev", 0x58000, 0x100, &dev);
    CHECK(bus.mem_read16(0x580FE) == 0xBEEF);   // both bytes inside: one word
    dev.log.clear();
    CHECK(bus.mem_read16(0x57FFE) == 0xFFFF);   // open bus below
    CHECK(dev.log.empty());
}
