// SPDX-License-Identifier: GPL-3.0-or-later
// The 80186's peripheral control block, against the behaviour the Intel
// 80C186 data sheet documents.
#include "bus/bus.hpp"
#include "hw/i80186_peripherals.hpp"

#include <doctest/doctest.h>

using namespace em34xx;

TEST_CASE("80186 PCB: starts at I/O FF00 and relocates to memory") {
    Bus bus(20);
    bus.set_policy(UnmappedPolicy::Ignore);
    I80186Peripherals pcb(bus);
    CHECK(bus.io_read16(0xFFFE) == 0x20FF);
    // The 3483's start-up: OUT FFFE,18FF moves the block to memory 8FF00.
    bus.io_write16(0xFFFE, 0x18FF);
    CHECK(pcb.in_memory());
    CHECK(pcb.base() == 0x8FF00);
    CHECK(bus.io_read16(0xFFFE) == 0xFFFF);   // gone from I/O
    CHECK(bus.mem_read16(0x8FFFE) == 0x18FF);
    bus.mem_write16(0x8FF28, 0x00ED);         // IMASK through the new window
    CHECK(pcb.read_reg(0x28) == 0x00ED);
    CHECK(bus.mem_read8(0x8FF28) == 0xED);    // byte reads give a half
    // Back to I/O at another base.
    bus.mem_write16(0x8FFFE, 0x0F00 >> 0);
    CHECK_FALSE(pcb.in_memory());
    CHECK(bus.mem_read16(0x8FFFE) == 0xFFFF);
}

TEST_CASE("80186 interrupts: priority, nesting, EOI, edge and level inputs") {
    Bus bus(20);
    I80186Peripherals pcb(bus);
    CHECK_FALSE(pcb.intr_asserted());
    pcb.write_reg(0x38, 0x0002);   // INT0: edge, unmasked, priority 2
    pcb.write_reg(0x3C, 0x0011);   // INT2: level, unmasked, priority 1
    pcb.set_int_line(0, true);
    REQUIRE(pcb.intr_asserted());
    CHECK(pcb.read_reg(0x26) == 0x800C);   // poll status: INT0, not acknowledged
    CHECK(pcb.acknowledge() == 0x0C);
    CHECK(pcb.read_reg(0x2C) == 0x10);     // INT0 in service
    CHECK_FALSE(pcb.intr_asserted());      // edge request consumed
    // Higher priority nests over the source in service.
    pcb.set_int_line(2, true);
    REQUIRE(pcb.intr_asserted());
    CHECK(pcb.acknowledge() == 0x0E);
    // Level request stays while the pin is high, but INT2 is in service.
    CHECK_FALSE(pcb.intr_asserted());
    pcb.write_reg(0x22, 0x000E);           // specific EOI for INT2
    CHECK(pcb.intr_asserted());            // pin still high
    pcb.set_int_line(2, false);
    CHECK_FALSE(pcb.intr_asserted());
    pcb.write_reg(0x22, 0x8000);           // non-specific: INT0
    CHECK(pcb.read_reg(0x2C) == 0);
    // Masked through IMASK; priority mask.
    pcb.set_int_line(0, false);
    pcb.set_int_line(0, true);
    pcb.write_reg(0x28, 0x0010);
    CHECK_FALSE(pcb.intr_asserted());
    pcb.write_reg(0x28, 0x0000);
    pcb.write_reg(0x2A, 0x0001);           // only priority 0-1
    CHECK_FALSE(pcb.intr_asserted());
    pcb.write_reg(0x2A, 0x0007);
    CHECK(pcb.read_reg(0x24) == 0x800C);   // POLL acknowledges
    CHECK(pcb.read_reg(0x2C) == 0x10);
}

TEST_CASE("80186 timers: count at a quarter of the CPU clock, interrupt, single and alternate modes") {
    Bus bus(20);
    I80186Peripherals pcb(bus);
    pcb.write_reg(0x32, 0x0000);           // timer interrupts unmasked
    pcb.write_reg(0x52, 10);               // timer 0 max A
    pcb.write_reg(0x56, 0xE001);           // EN|INH|INT|CONT
    pcb.tick(39);
    CHECK(pcb.read_reg(0x50) == 9);
    CHECK_FALSE(pcb.intr_asserted());
    pcb.tick(1);
    CHECK(pcb.read_reg(0x50) == 0);
    CHECK((pcb.read_reg(0x56) & 0x0020) != 0);   // MC
    REQUIRE(pcb.intr_asserted());
    CHECK(pcb.acknowledge() == 0x08);
    pcb.write_reg(0x22, 0x0008);

    // EN changes only with INH.
    pcb.write_reg(0x56, 0x0000);
    CHECK((pcb.read_reg(0x56) & 0x8000) != 0);
    pcb.write_reg(0x56, 0x4000);
    CHECK((pcb.read_reg(0x56) & 0x8000) == 0);

    // Timer 1, single cycle, alternate A/B: stops after B.
    pcb.write_reg(0x5A, 2);
    pcb.write_reg(0x5C, 3);
    pcb.write_reg(0x5E, 0xE002);           // EN|INH|INT|ALT, not continuous
    pcb.tick(4 * 2);
    CHECK((pcb.read_reg(0x5E) & 0x9000) == 0x9000);   // running, now on B
    pcb.tick(4 * 3);
    CHECK((pcb.read_reg(0x5E) & 0x8000) == 0);        // stopped
    CHECK(pcb.acknowledge() == 0x12);

    // Timer 2 prescales timer 0.
    pcb.write_reg(0x22, 0x8000);
    pcb.write_reg(0x50, 0);
    pcb.write_reg(0x62, 5);
    pcb.write_reg(0x66, 0xC001);           // timer 2 EN|INH|CONT, no interrupt
    pcb.write_reg(0x52, 3);
    pcb.write_reg(0x56, 0xC009);           // timer 0 EN|INH|P|CONT
    pcb.tick(4 * 5 * 2);
    CHECK(pcb.read_reg(0x50) == 2);
}

TEST_CASE("80186 cascade mode takes the vector from the external controller") {
    struct Slave : InterruptController {
        bool intr_asserted() const override { return true; }
        uint8_t acknowledge() override { return 0x40; }
    } slave;
    Bus bus(20);
    I80186Peripherals pcb(bus);
    pcb.write_reg(0x38, 0x0030);           // INT0: cascade, level, priority 0
    pcb.set_cascade_controller(0, &slave);
    pcb.set_int_line(0, true);
    CHECK(pcb.acknowledge() == 0x40);
    CHECK(pcb.read_reg(0x2C) == 0x10);
}

TEST_CASE("80186 DMA and chip-select registers are kept") {
    Bus bus(20);
    I80186Peripherals pcb(bus);
    pcb.write_reg(0xC0, 0x1234);
    pcb.write_reg(0xDA, 0x0005);
    pcb.write_reg(0xA2, 0x3FF8);
    CHECK(pcb.read_reg(0xC0) == 0x1234);
    CHECK(pcb.read_reg(0xDA) == 0x0005);
    CHECK(pcb.read_reg(0xA2) == 0x3FF8);
    CHECK(pcb.describe().find("LCS 00000-3ffff") != std::string::npos);
}
