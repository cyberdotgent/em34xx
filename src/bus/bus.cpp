// SPDX-License-Identifier: GPL-3.0-or-later
#include "bus/bus.hpp"

#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>
#include <stdexcept>

namespace em34xx {

const char* unmapped_policy_name(UnmappedPolicy p) {
    switch (p) {
    case UnmappedPolicy::Stop: return "stop";
    case UnmappedPolicy::Log: return "log";
    case UnmappedPolicy::Ignore: return "ignore";
    }
    return "?";
}

bool parse_unmapped_policy(const std::string& text, UnmappedPolicy& out) {
    const std::string t = to_lower(text);
    if (t == "stop") out = UnmappedPolicy::Stop;
    else if (t == "log") out = UnmappedPolicy::Log;
    else if (t == "ignore") out = UnmappedPolicy::Ignore;
    else return false;
    return true;
}

std::string BusStop::describe() const {
    const char* dir = write ? "write" : "read";
    switch (cause) {
    case Cause::None: return "none";
    case Cause::Unmapped:
        if (io) return format("unmapped I/O %s port %04x%s", dir, address,
                              write ? format(" value %02x", value).c_str() : "");
        return format("unmapped memory %s at %06x%s", dir, address,
                      write ? format(" value %02x", value).c_str() : "");
    case Cause::RomWrite:
        return format("write to ROM at %06x value %02x", address, value);
    case Cause::Watchpoint:
        return format("watchpoint %d: %s at %06x value %02x", watch_id, dir, address, value);
    }
    return "?";
}

Bus::Bus(unsigned address_bits)
    : address_bits_(address_bits),
      address_mask_(address_bits >= 32 ? 0xFFFFFFFFu : ((1u << address_bits) - 1)),
      pages_((size_t(address_mask_) + 1) >> kPageShift, nullptr),
      io_ports_(0x10000, nullptr) {}

static void check_alignment(uint32_t base, uint32_t size) {
    if ((base & 0xFF) || (size & 0xFF) || size == 0)
        throw std::invalid_argument(format("mapping %06x+%x is not 256-byte aligned", base, size));
}

Region& Bus::map_ram(const std::string& name, uint32_t base, uint32_t size) {
    check_alignment(base, size);
    auto r = std::make_unique<Region>();
    r->name = name;
    r->kind = Region::Kind::Ram;
    r->base = base;
    r->size = size;
    r->storage.assign(size, 0);
    regions_.push_back(std::move(r));
    rebuild_pages();
    return *regions_.back();
}

Region& Bus::map_rom(const std::string& name, uint32_t base, std::vector<uint8_t> data) {
    check_alignment(base, uint32_t(data.size()));
    auto r = std::make_unique<Region>();
    r->name = name;
    r->kind = Region::Kind::Rom;
    r->base = base;
    r->size = uint32_t(data.size());
    r->storage = std::move(data);
    regions_.push_back(std::move(r));
    rebuild_pages();
    return *regions_.back();
}

Region& Bus::map_mmio(const std::string& name, uint32_t base, uint32_t size, Device* device) {
    check_alignment(base, size);
    auto r = std::make_unique<Region>();
    r->name = name;
    r->kind = Region::Kind::Mmio;
    r->base = base;
    r->size = size;
    r->device = device;
    regions_.push_back(std::move(r));
    rebuild_pages();
    return *regions_.back();
}

void Bus::map_io(const std::string& name, uint16_t base, uint32_t count, Device* device) {
    io_.push_back(IoMapping{name, base, count, device});
    for (uint32_t p = base; p < uint32_t(base) + count && p < 0x10000; ++p) io_ports_[p] = device;
}

void Bus::unmap(uint32_t base, uint32_t size) {
    const uint64_t end = uint64_t(base) + size;
    regions_.erase(std::remove_if(regions_.begin(), regions_.end(),
                                  [&](const std::unique_ptr<Region>& r) {
                                      return r->base < end && uint64_t(r->base) + r->size > base;
                                  }),
                   regions_.end());
    rebuild_pages();
}

void Bus::rebuild_pages() {
    std::fill(pages_.begin(), pages_.end(), nullptr);
    // Later mappings take precedence over earlier ones.
    for (auto& r : regions_) {
        for (uint64_t a = r->base; a < uint64_t(r->base) + r->size && a <= address_mask_; a += 1u << kPageShift)
            pages_[size_t(a >> kPageShift)] = r.get();
    }
}

const Region* Bus::region_at(uint32_t addr) const {
    return lookup(addr & address_mask_);
}

void Bus::unexpected(bool io, bool write, uint32_t addr, uint8_t value, BusStop::Cause cause) {
    const uint64_t key = (uint64_t(io) << 40) | (uint64_t(write) << 39) | addr;
    UnmappedRecord& rec = unmapped_[key];
    if (rec.count == 0) rec.first_pc = pc_;
    ++rec.count;
    rec.last_value = value;

    if (policy_ == UnmappedPolicy::Ignore) return;
    BusStop s;
    s.cause = cause;
    s.io = io;
    s.write = write;
    s.address = addr;
    s.value = value;
    if (policy_ == UnmappedPolicy::Log) {
        LOG_WARN("bus", "pc %06x: %s", pc_, s.describe().c_str());
        return;
    }
    // Stop policy: stop the first time each distinct address/direction is
    // touched. Repeats are only counted, so "continue" makes progress
    // through polling loops; the "unmapped" report shows the totals.
    if (rec.count == 1) {
        LOG_WARN("bus", "pc %06x: %s", pc_, s.describe().c_str());
        if (!stop_pending()) stop_ = s;
    }
}

void Bus::check_watch(uint32_t addr, bool write, uint8_t value) {
    for (const Watchpoint& w : watches_) {
        if (addr < w.start || addr >= w.start + w.length) continue;
        const uint8_t want = write ? uint8_t(AccessKind::Write) : uint8_t(AccessKind::Read);
        if (!(uint8_t(w.kind) & want)) continue;
        if (!stop_pending()) {
            stop_.cause = BusStop::Cause::Watchpoint;
            stop_.write = write;
            stop_.address = addr;
            stop_.value = value;
            stop_.watch_id = w.id;
        }
        return;
    }
}

uint8_t Bus::mem_read8(uint32_t phys) {
    phys &= address_mask_;
    Region* r = lookup(phys);
    uint8_t v;
    if (!r) {
        v = 0xFF;   // open bus
        unexpected(false, false, phys, v, BusStop::Cause::Unmapped);
    } else if (r->kind == Region::Kind::Mmio) {
        v = r->device->mmio_read8(phys - r->base);
        LOG_TRACE("mmio", "pc %06x: rd %s+%x -> %02x", pc_, r->name.c_str(), phys - r->base, v);
    } else {
        v = r->storage[phys - r->base];
    }
    if (!watches_.empty()) check_watch(phys, false, v);
    return v;
}

void Bus::mem_write8(uint32_t phys, uint8_t value) {
    phys &= address_mask_;
    Region* r = lookup(phys);
    if (!r) {
        unexpected(false, true, phys, value, BusStop::Cause::Unmapped);
    } else if (r->kind == Region::Kind::Ram) {
        r->storage[phys - r->base] = value;
    } else if (r->kind == Region::Kind::Mmio) {
        LOG_TRACE("mmio", "pc %06x: wr %s+%x <- %02x", pc_, r->name.c_str(), phys - r->base, value);
        r->device->mmio_write8(phys - r->base, value);
    } else {
        unexpected(false, true, phys, value, BusStop::Cause::RomWrite);
    }
    note_write(phys);
    if (!watches_.empty()) check_watch(phys, true, value);
}

uint8_t Bus::io_read8(uint16_t port) {
    if (Device* d = io_ports_[port]) {
        uint8_t v = d->io_read8(port);
        LOG_TRACE("io", "pc %06x: in %04x -> %02x (%s)", pc_, port, v, d->name().c_str());
        return v;
    }
    unexpected(true, false, port, 0xFF, BusStop::Cause::Unmapped);
    return 0xFF;
}

void Bus::io_write8(uint16_t port, uint8_t value) {
    if (Device* d = io_ports_[port]) {
        LOG_TRACE("io", "pc %06x: out %04x <- %02x (%s)", pc_, port, value, d->name().c_str());
        d->io_write8(port, value);
        return;
    }
    unexpected(true, true, port, value, BusStop::Cause::Unmapped);
}

uint16_t Bus::mem_read16(uint32_t phys) {
    phys &= address_mask_;
    const uint32_t hi_addr = (phys + 1) & address_mask_;
    Region* r = lookup(phys);
    if (!r || r->kind != Region::Kind::Mmio || lookup(hi_addr) != r || hi_addr != phys + 1)
        return CpuBus::mem_read16(phys);
    const uint16_t v = r->device->mmio_read16(phys - r->base);
    LOG_TRACE("mmio", "pc %06x: rd %s+%x -> %04x", pc_, r->name.c_str(), phys - r->base, v);
    if (!watches_.empty()) {
        check_watch(phys, false, uint8_t(v));
        check_watch(hi_addr, false, uint8_t(v >> 8));
    }
    return v;
}

void Bus::mem_write16(uint32_t phys, uint16_t value) {
    phys &= address_mask_;
    const uint32_t hi_addr = (phys + 1) & address_mask_;
    Region* r = lookup(phys);
    if (!r || r->kind != Region::Kind::Mmio || lookup(hi_addr) != r || hi_addr != phys + 1) {
        CpuBus::mem_write16(phys, value);
        return;
    }
    LOG_TRACE("mmio", "pc %06x: wr %s+%x <- %04x", pc_, r->name.c_str(), phys - r->base, value);
    r->device->mmio_write16(phys - r->base, value);
    note_write(phys);
    note_write(hi_addr);
    if (!watches_.empty()) {
        check_watch(phys, true, uint8_t(value));
        check_watch(hi_addr, true, uint8_t(value >> 8));
    }
}

uint16_t Bus::io_read16(uint16_t port) {
    Device* d = io_ports_[port];
    if (!d || port == 0xFFFF || io_ports_[port + 1] != d) return CpuBus::io_read16(port);
    const uint16_t v = d->io_read16(port);
    LOG_TRACE("io", "pc %06x: in %04x -> %04x (%s)", pc_, port, v, d->name().c_str());
    return v;
}

void Bus::io_write16(uint16_t port, uint16_t value) {
    Device* d = io_ports_[port];
    if (!d || port == 0xFFFF || io_ports_[port + 1] != d) {
        CpuBus::io_write16(port, value);
        return;
    }
    LOG_TRACE("io", "pc %06x: out %04x <- %04x (%s)", pc_, port, value, d->name().c_str());
    d->io_write16(port, value);
}

uint8_t Bus::interrupt_ack() {
    return ic_ ? ic_->acknowledge() : 0xFF;
}

uint8_t Bus::peek8(uint32_t phys) const {
    phys &= address_mask_;
    Region* r = lookup(phys);
    if (!r) return 0xFF;
    if (r->kind == Region::Kind::Mmio) return r->device->peek8(phys - r->base);
    return r->storage[phys - r->base];
}

bool Bus::poke8(uint32_t phys, uint8_t value) {
    phys &= address_mask_;
    Region* r = lookup(phys);
    if (!r || r->kind != Region::Kind::Ram) return false;
    r->storage[phys - r->base] = value;
    note_write(phys);
    return true;
}

void Bus::observe_writes(uint32_t base, uint32_t size) {
    if (observed_pages_.empty()) observed_pages_.assign(pages_.size(), 0);
    for (uint64_t a = base & address_mask_; a < uint64_t(base) + size && a <= address_mask_; a += 1u << kPageShift)
        observed_pages_[size_t(a >> kPageShift)] = 1;
}

int Bus::add_watchpoint(uint32_t start, uint32_t length, AccessKind kind) {
    Watchpoint w;
    w.id = next_watch_id_++;
    w.start = start & address_mask_;
    w.length = length ? length : 1;
    w.kind = kind;
    watches_.push_back(w);
    return w.id;
}

bool Bus::remove_watchpoint(int id) {
    auto it = std::find_if(watches_.begin(), watches_.end(), [&](const Watchpoint& w) { return w.id == id; });
    if (it == watches_.end()) return false;
    watches_.erase(it);
    return true;
}

bool Bus::remove_watchpoint_at(uint32_t start, uint32_t length, AccessKind kind) {
    auto it = std::find_if(watches_.begin(), watches_.end(), [&](const Watchpoint& w) {
        return w.start == start && w.length == (length ? length : 1) && w.kind == kind;
    });
    if (it == watches_.end()) return false;
    watches_.erase(it);
    return true;
}

void Bus::reset_devices() {
    std::vector<Device*> seen;
    auto reset_once = [&](Device* d) {
        if (d && std::find(seen.begin(), seen.end(), d) == seen.end()) {
            seen.push_back(d);
            d->reset();
        }
    };
    for (auto& r : regions_) reset_once(r->device);
    for (auto& m : io_) reset_once(m.device);
}

} // namespace em34xx
