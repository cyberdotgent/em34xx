// SPDX-License-Identifier: GPL-3.0-or-later
// The integrated peripherals of an Intel 80C186-class CPU: the peripheral
// control block (PCB) and what it controls.
//
// Documented (Intel 80C186 data sheet): a 256-byte block of word registers
// at I/O FF00 after reset, which the relocation register (PCB+FE) moves to
// any 256-byte boundary in I/O or memory space; an interrupt controller
// (master mode) with timer, two DMA and four external sources; three
// 16-bit timers; two DMA channels; and the chip-select unit.
//
// What is modelled:
//  - relocation (I/O and memory), word and byte access;
//  - the interrupt controller in master mode: mask, priority mask,
//    in-service, request, poll, specific and non-specific EOI, priority
//    resolution with nesting, edge- and level-triggered external inputs,
//    cascade mode on INT0/INT1 with the vector from an external controller
//    when one is attached;
//  - the timers: internal clock (CPU clock / 4), timer 2 prescaling of
//    timers 0/1, alternate compare, continuous/single, interrupts, MC;
//  - DMA and chip-select registers as storage (describe() decodes the
//    chip-select windows). DMA transfers are not modelled yet: starting a
//    channel is logged once. External timer inputs and outputs are not
//    modelled.
//
// Vector types (documented): timer 0 = 08h, DMA0 = 0Ah, DMA1 = 0Bh,
// INT0-INT3 = 0Ch-0Fh, timer 1 = 12h, timer 2 = 13h.
#pragma once

#include "bus/device.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace em34xx {

class Bus;

class I80186Peripherals : public Device, public InterruptController {
public:
    // The bus the block maps itself into; it starts at I/O FF00.
    explicit I80186Peripherals(Bus& bus);

    // External interrupt input pins INT0-INT3.
    void set_int_line(unsigned n, bool level);
    // In cascade mode INT0/INT1 take their vector from an external
    // controller (an 8259A slave on the real part). Null: the source's own
    // type, as if the slave returned it.
    void set_cascade_controller(unsigned n, InterruptController* ic) { cascade_[n & 1] = ic; }

    // Where the block is now (for the monitor and tests).
    bool in_memory() const { return (relocation_ & 0x1000) != 0; }
    uint32_t base() const { return uint32_t(relocation_ & 0x0FFF) << 8; }

    // Device
    uint8_t io_read8(uint16_t port) override;
    void io_write8(uint16_t port, uint8_t value) override;
    uint16_t io_read16(uint16_t port) override { return read_reg(port & 0xFF); }
    void io_write16(uint16_t port, uint16_t value) override { write_reg(port & 0xFF, value); }
    uint8_t mmio_read8(uint32_t offset) override;
    void mmio_write8(uint32_t offset, uint8_t value) override;
    uint16_t mmio_read16(uint32_t offset) override { return read_reg(offset & 0xFF); }
    void mmio_write16(uint32_t offset, uint16_t value) override { write_reg(offset & 0xFF, value); }
    uint8_t peek8(uint32_t offset) override;
    void reset() override;
    void tick(uint64_t cycles) override;
    std::string describe() const override;

    // InterruptController
    bool intr_asserted() const override;
    uint8_t acknowledge() override;

    // Register access by PCB offset (even), without side effects of
    // relocation; for tests and the monitor.
    uint16_t read_reg(unsigned offset);
    void write_reg(unsigned offset, uint16_t value);

private:
    // Interrupt sources in the controller's fixed order (bit numbers of the
    // mask, in-service and request registers).
    enum Src { kTimer = 0, kDma0 = 2, kDma1 = 3, kInt0 = 4, kInt1 = 5, kInt2 = 6, kInt3 = 7 };

    struct Timer {
        uint16_t count = 0;
        uint16_t max_a = 0;
        uint16_t max_b = 0;
        uint16_t control = 0;
    };
    struct DmaChannel {
        uint16_t src_lo = 0, src_hi = 0, dst_lo = 0, dst_hi = 0, count = 0, control = 0;
    };

    void remap();
    void unmap();
    uint16_t source_control(unsigned src) const;
    unsigned source_priority(unsigned src) const { return source_control(src) & 7; }
    bool source_masked(unsigned src) const { return (source_control(src) & 0x08) != 0; }
    uint16_t request_bits() const;
    // The highest-priority request that may interrupt now, or -1.
    int pending_source() const;
    uint8_t vector_for(unsigned src) const;
    // Acknowledge `src` (INTA or POLL): in service, request consumed.
    uint8_t take(unsigned src);
    void eoi(uint16_t value);
    void count_timer(unsigned n, uint64_t increments);
    void timer_reached_max(unsigned n);
    void write_timer_control(unsigned n, uint16_t value);

    Bus& bus_;
    uint16_t relocation_ = 0x20FF;
    bool mapped_ = false;
    bool mapped_memory_ = false;
    uint32_t mapped_base_ = 0;

    // Interrupt controller.
    std::array<uint16_t, 8> control_{};   // per source bit (index 1 unused); timer = TCUCON
    uint16_t priority_mask_ = 7;
    uint16_t in_service_ = 0;
    uint16_t latched_ = 0;                // edge-triggered external requests and DMA
    std::array<bool, 4> pin_{};
    uint16_t timer_requests_ = 0;         // INTSTS bits 2-0: timers 0-2
    uint8_t timer_in_service_ = 0;        // which timer the timer in-service bit is for
    InterruptController* cascade_[2] = {nullptr, nullptr};

    std::array<Timer, 3> timers_{};
    uint64_t clock_residue_ = 0;          // CPU clocks not yet worth a timer increment

    std::array<DmaChannel, 2> dma_{};
    bool dma_logged_ = false;

    // Chip selects: UMCS, LMCS, PACS, MMCS, MPCS.
    std::array<uint16_t, 5> chip_select_{};
};

} // namespace em34xx
