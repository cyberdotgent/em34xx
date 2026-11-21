// SPDX-License-Identifier: GPL-3.0-or-later
#include "bus/bus.hpp"

#include <doctest/doctest.h>

using namespace tn34xx;

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
