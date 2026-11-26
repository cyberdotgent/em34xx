// SPDX-License-Identifier: GPL-3.0-or-later
// Interface between an x86-family CPU core and the machine it lives in.
#pragma once

#include <cstdint>

namespace em34xx {

// The CPU sees physical memory and I/O ports only through this interface.
// A CPU with an 8-bit data bus (8088, V20) splits every word into byte
// accesses. One with a 16-bit bus (8086, 80186, 80286) issues an aligned
// word as one word access, which the bus can hand to a device whose
// registers are word-wide; an odd-aligned word is two byte accesses, as on
// the real bus. The word calls default to two byte accesses (low byte
// first), so an implementation only needs byte granularity.
class CpuBus {
public:
    virtual ~CpuBus() = default;

    virtual uint8_t mem_read8(uint32_t phys) = 0;
    virtual void mem_write8(uint32_t phys, uint8_t value) = 0;

    virtual uint8_t io_read8(uint16_t port) = 0;
    virtual void io_write8(uint16_t port, uint8_t value) = 0;

    // One aligned word access on a 16-bit bus (phys and port are even).
    virtual uint16_t mem_read16(uint32_t phys) {
        const uint16_t lo = mem_read8(phys);
        return uint16_t(lo | (mem_read8(phys + 1) << 8));
    }
    virtual void mem_write16(uint32_t phys, uint16_t value) {
        mem_write8(phys, uint8_t(value));
        mem_write8(phys + 1, uint8_t(value >> 8));
    }
    virtual uint16_t io_read16(uint16_t port) {
        const uint16_t lo = io_read8(port);
        return uint16_t(lo | (io_read8(uint16_t(port + 1)) << 8));
    }
    virtual void io_write16(uint16_t port, uint16_t value) {
        io_write8(port, uint8_t(value));
        io_write8(uint16_t(port + 1), uint8_t(value >> 8));
    }

    // Instruction fetches go through a separate hook so that execute
    // breakpoints and fetch tracing can tell them apart from data reads.
    virtual uint8_t fetch8(uint32_t phys) { return mem_read8(phys); }

    // Interrupt acknowledge cycle: returns the vector supplied by the
    // interrupt controller for the currently asserted INTR line.
    virtual uint8_t interrupt_ack() { return 0xFF; }
};

} // namespace em34xx
