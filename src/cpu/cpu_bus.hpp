// SPDX-License-Identifier: GPL-3.0-or-later
// Interface between an x86-family CPU core and the machine it lives in.
#pragma once

#include <cstdint>

namespace tn34xx {

// The CPU sees physical memory and I/O ports only through this interface.
// Word accesses are split into byte accesses by the CPU core according to
// its bus width; implementations only need byte granularity.
class CpuBus {
public:
    virtual ~CpuBus() = default;

    virtual uint8_t mem_read8(uint32_t phys) = 0;
    virtual void mem_write8(uint32_t phys, uint8_t value) = 0;

    virtual uint8_t io_read8(uint16_t port) = 0;
    virtual void io_write8(uint16_t port, uint8_t value) = 0;

    // Instruction fetches go through a separate hook so that execute
    // breakpoints and fetch tracing can tell them apart from data reads.
    virtual uint8_t fetch8(uint32_t phys) { return mem_read8(phys); }

    // Interrupt acknowledge cycle: returns the vector supplied by the
    // interrupt controller for the currently asserted INTR line.
    virtual uint8_t interrupt_ack() { return 0xFF; }
};

} // namespace tn34xx
