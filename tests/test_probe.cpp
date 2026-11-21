// SPDX-License-Identifier: GPL-3.0-or-later
// Probe devices: the monitor's stand-ins for unknown hardware (devtools).
#include "bus/bus.hpp"
#include "devtools/probe_device.hpp"

#include <doctest/doctest.h>

using namespace em34xx;

TEST_CASE("probe device latches and peeks without side effects") {
    Bus bus(20);
    ProbeDevice probe("p", 0x100, true, 0xFF);
    bus.map_mmio("p", 0xA0000, 0x100, &probe);
    bus.mem_write8(0xA0010, 0x55);
    CHECK(bus.mem_read8(0xA0010) == 0x55);
    CHECK(bus.peek8(0xA0010) == 0x55);
    CHECK(bus.peek8(0xA0011) == 0xFF);
}

TEST_CASE("later mappings override earlier ones; unmap removes") {
    Bus bus(20);
    bus.map_ram("low", 0, 0x10000);
    ProbeDevice probe("p", 0x100, false, 0xAB);
    bus.map_mmio("p", 0x8000, 0x100, &probe);
    CHECK(bus.peek8(0x8000) == 0xAB);
    bus.unmap(0x8000, 0x100);
    // The RAM region overlapped and was removed as well.
    CHECK(bus.region_at(0x0) == nullptr);
}
