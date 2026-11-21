// SPDX-License-Identifier: GPL-3.0-or-later
// Physical memory and I/O space of an emulated machine.
//
// Memory is decoded in 256-byte pages. Each page points to a region: RAM,
// ROM, or a memory-mapped device. Accesses to undecoded addresses (and
// writes to ROM) are "unexpected accesses" handled by a configurable policy.
// During bring-up the default is to stop, so every hardware access the
// firmware makes that we have not modelled becomes visible immediately.
#pragma once

#include "bus/device.hpp"
#include "cpu/cpu_bus.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tn34xx {

enum class UnmappedPolicy { Stop, Log, Ignore };
const char* unmapped_policy_name(UnmappedPolicy p);
bool parse_unmapped_policy(const std::string& text, UnmappedPolicy& out);

enum class AccessKind : uint8_t { Read = 1, Write = 2, ReadWrite = 3 };

struct Region {
    enum class Kind { Ram, Rom, Mmio };
    std::string name;
    Kind kind = Kind::Ram;
    uint32_t base = 0;
    uint32_t size = 0;
    std::vector<uint8_t> storage;   // RAM/ROM contents
    Device* device = nullptr;       // MMIO target
};

struct IoMapping {
    std::string name;
    uint16_t base = 0;
    uint32_t count = 0;
    Device* device = nullptr;
};

struct Watchpoint {
    int id = 0;
    uint32_t start = 0;
    uint32_t length = 1;
    AccessKind kind = AccessKind::Write;
};

// Why the bus asked the machine to stop after the current instruction.
struct BusStop {
    enum class Cause { None, Unmapped, RomWrite, Watchpoint };
    Cause cause = Cause::None;
    bool io = false;
    bool write = false;
    uint32_t address = 0;
    uint8_t value = 0;
    int watch_id = 0;
    std::string describe() const;
};

// One distinct unexpected access, aggregated for the "unmapped" report.
struct UnmappedRecord {
    uint64_t count = 0;
    uint32_t first_pc = 0;
    uint8_t last_value = 0;
};

class Bus : public CpuBus {
public:
    explicit Bus(unsigned address_bits);

    unsigned address_bits() const { return address_bits_; }
    uint32_t address_mask() const { return address_mask_; }

    // --- mapping -----------------------------------------------------
    Region& map_ram(const std::string& name, uint32_t base, uint32_t size);
    Region& map_rom(const std::string& name, uint32_t base, std::vector<uint8_t> data);
    Region& map_mmio(const std::string& name, uint32_t base, uint32_t size, Device* device);
    void map_io(const std::string& name, uint16_t base, uint32_t count, Device* device);
    // Remove every region overlapping [base, base+size).
    void unmap(uint32_t base, uint32_t size);
    const std::vector<std::unique_ptr<Region>>& regions() const { return regions_; }
    const std::vector<IoMapping>& io_mappings() const { return io_; }
    const Region* region_at(uint32_t addr) const;

    // --- CpuBus ------------------------------------------------------
    uint8_t mem_read8(uint32_t phys) override;
    void mem_write8(uint32_t phys, uint8_t value) override;
    uint8_t io_read8(uint16_t port) override;
    void io_write8(uint16_t port, uint8_t value) override;
    uint8_t interrupt_ack() override;

    void set_interrupt_controller(InterruptController* ic) { ic_ = ic; }
    InterruptController* interrupt_controller() const { return ic_; }

    // --- debugger access (no side effects, no policy, no watchpoints) --
    uint8_t peek8(uint32_t phys) const;
    // Writes RAM only; returns false for ROM/MMIO/unmapped addresses.
    bool poke8(uint32_t phys, uint8_t value);

    // --- unexpected-access policy -------------------------------------
    void set_policy(UnmappedPolicy p) { policy_ = p; }
    UnmappedPolicy policy() const { return policy_; }
    const std::map<uint64_t, UnmappedRecord>& unmapped_log() const { return unmapped_; }
    void clear_unmapped_log() { unmapped_.clear(); }

    // The machine sets the address of the executing instruction so that
    // log lines and records can attribute accesses.
    void set_pc_context(uint32_t pc) { pc_ = pc; }

    // --- stop requests -------------------------------------------------
    bool stop_pending() const { return stop_.cause != BusStop::Cause::None; }
    const BusStop& stop_info() const { return stop_; }
    void clear_stop() { stop_ = BusStop{}; }

    // --- watchpoints ---------------------------------------------------
    int add_watchpoint(uint32_t start, uint32_t length, AccessKind kind);
    bool remove_watchpoint(int id);
    bool remove_watchpoint_at(uint32_t start, uint32_t length, AccessKind kind);
    void clear_watchpoints() { watches_.clear(); }
    const std::vector<Watchpoint>& watchpoints() const { return watches_; }

    void reset_devices();

    // --- write observation ---------------------------------------------
    // Count writes (CPU and debugger) into the given 256-byte pages, e.g. a
    // display's window, character generator and registers, so the machine
    // can tell cheaply whether the picture may have changed.
    void observe_writes(uint32_t base, uint32_t size);
    uint64_t observed_writes() const { return observed_writes_; }

private:
    static constexpr unsigned kPageShift = 8;

    Region* lookup(uint32_t addr) const { return pages_[addr >> kPageShift]; }
    void rebuild_pages();
    void unexpected(bool io, bool write, uint32_t addr, uint8_t value, BusStop::Cause cause);
    void check_watch(uint32_t addr, bool write, uint8_t value);

    unsigned address_bits_;
    uint32_t address_mask_;
    std::vector<std::unique_ptr<Region>> regions_;
    std::vector<Region*> pages_;
    std::vector<IoMapping> io_;
    std::vector<Device*> io_ports_;   // 64K entries
    InterruptController* ic_ = nullptr;

    UnmappedPolicy policy_ = UnmappedPolicy::Stop;
    std::map<uint64_t, UnmappedRecord> unmapped_;
    uint32_t pc_ = 0;
    BusStop stop_;

    std::vector<Watchpoint> watches_;
    int next_watch_id_ = 1;

    std::vector<uint8_t> observed_pages_;   // empty until observe_writes()
    uint64_t observed_writes_ = 0;

    void note_write(uint32_t phys) {
        if (!observed_pages_.empty() && observed_pages_[phys >> kPageShift]) ++observed_writes_;
    }
};

} // namespace tn34xx
