// SPDX-License-Identifier: GPL-3.0-or-later
// Base class for emulated hardware attached to the bus.
#pragma once

#include <cstdint>
#include <string>

namespace em34xx {

class Device {
public:
    explicit Device(std::string name) : name_(std::move(name)) {}
    virtual ~Device() = default;

    const std::string& name() const { return name_; }

    // Memory-mapped access; offset is relative to the start of the mapping.
    virtual uint8_t mmio_read8(uint32_t offset) { (void)offset; return 0xFF; }
    virtual void mmio_write8(uint32_t offset, uint8_t value) { (void)offset; (void)value; }

    // Port-mapped access; port is the absolute I/O port number.
    virtual uint8_t io_read8(uint16_t port) { (void)port; return 0xFF; }
    virtual void io_write8(uint16_t port, uint8_t value) { (void)port; (void)value; }

    // One aligned word cycle from a CPU with a 16-bit bus (offset and port
    // even, both bytes in this device). A device with word-wide registers
    // overrides these; the defaults are two byte accesses, low byte first.
    virtual uint16_t mmio_read16(uint32_t offset) {
        const uint16_t lo = mmio_read8(offset);
        return uint16_t(lo | (mmio_read8(offset + 1) << 8));
    }
    virtual void mmio_write16(uint32_t offset, uint16_t value) {
        mmio_write8(offset, uint8_t(value));
        mmio_write8(offset + 1, uint8_t(value >> 8));
    }
    virtual uint16_t io_read16(uint16_t port) {
        const uint16_t lo = io_read8(port);
        return uint16_t(lo | (io_read8(uint16_t(port + 1)) << 8));
    }
    virtual void io_write16(uint16_t port, uint16_t value) {
        io_write8(port, uint8_t(value));
        io_write8(uint16_t(port + 1), uint8_t(value >> 8));
    }

    // Side-effect-free read used by the debugger. Devices whose reads have
    // side effects should override this to return their latched state.
    virtual uint8_t peek8(uint32_t offset) { (void)offset; return 0xFF; }

    virtual void reset() {}

    // Advance device time by the given number of CPU clocks.
    virtual void tick(uint64_t cycles) { (void)cycles; }

    // Human-readable state for the monitor's "dev" command.
    virtual std::string describe() const { return {}; }

    // Write any backing file (battery-backed RAM, an EEPROM image) now, as
    // at power-off. False with error when it cannot be written.
    virtual bool flush_nonvolatile(std::string& error) { (void)error; return true; }

private:
    std::string name_;
};

// Interrupt controller as seen by the CPU: an INTR line and an acknowledge
// cycle that yields a vector.
class InterruptController {
public:
    virtual ~InterruptController() = default;
    virtual bool intr_asserted() const = 0;
    virtual uint8_t acknowledge() = 0;
};

} // namespace em34xx
