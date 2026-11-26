// SPDX-License-Identifier: GPL-3.0-or-later
// Register behaviour from the Intel 80C186 data sheet (documented); see the
// header for what is and is not modelled.
#include "hw/i80186_peripherals.hpp"

#include "bus/bus.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>

namespace em34xx {

namespace {

// PCB offsets (documented).
constexpr unsigned kEoi = 0x22, kPoll = 0x24, kPollStatus = 0x26, kImask = 0x28, kPriMask = 0x2A,
                   kInService = 0x2C, kRequest = 0x2E, kIntStatus = 0x30, kTimerCtl = 0x32, kDma0Ctl = 0x34,
                   kDma1Ctl = 0x36, kInt0Ctl = 0x38, kInt3Ctl = 0x3E;
constexpr unsigned kTimer0 = 0x50, kTimerEnd = 0x68;
constexpr unsigned kUmcs = 0xA0, kMpcs = 0xA8;
constexpr unsigned kDma0Regs = 0xC0, kDma1Regs = 0xD0, kDmaEnd = 0xDC;
constexpr unsigned kRelocation = 0xFE;

// Timer control bits.
constexpr uint16_t kEn = 0x8000, kInh = 0x4000, kInt = 0x2000, kRiu = 0x1000, kMc = 0x0020, kP = 0x0008,
                   kExt = 0x0004, kAlt = 0x0002, kCont = 0x0001;

// Source bit -> its control register offset.
unsigned control_offset(unsigned src) {
    switch (src) {
    case 0: return kTimerCtl;
    case 2: return kDma0Ctl;
    case 3: return kDma1Ctl;
    default: return kInt0Ctl + 2 * (src - 4);
    }
}

constexpr unsigned kSources[] = {0, 2, 3, 4, 5, 6, 7};

} // namespace

I80186Peripherals::I80186Peripherals(Bus& bus) : Device("i80186"), bus_(bus) { reset(); }

void I80186Peripherals::unmap() {
    if (!mapped_) return;
    if (mapped_memory_) bus_.unmap_device_mmio(this);
    else bus_.unmap_device_io(this);
    mapped_ = false;
}

void I80186Peripherals::remap() {
    const bool mem = in_memory();
    const uint32_t b = base();
    if (mapped_ && mem == mapped_memory_ && b == mapped_base_) return;
    unmap();
    if (mem) bus_.map_mmio("80186 PCB", b, 0x100, this);
    else bus_.map_io("80186 PCB", uint16_t(b), 0x100, this);
    mapped_ = true;
    mapped_memory_ = mem;
    mapped_base_ = b;
    LOG_DEBUG("i80186", "peripheral control block at %s %05x", mem ? "memory" : "I/O", b);
}

void I80186Peripherals::reset() {
    relocation_ = 0x20FF;   // I/O FF00, ESC trap (documented reset value)
    control_.fill(0x000F);  // masked, priority 7
    priority_mask_ = 7;
    in_service_ = 0;
    latched_ = 0;
    timer_requests_ = 0;
    timer_in_service_ = 0;
    timers_ = {};
    clock_residue_ = 0;
    dma_ = {};
    for (auto& d : dma_) d.control = 0xFFF9;   // documented: stopped, other bits undefined
    dma_logged_ = false;
    chip_select_ = {0xFFFB, 0, 0, 0, 0};       // UMCS: upper 1 KiB active at reset
    remap();
}

uint16_t I80186Peripherals::source_control(unsigned src) const { return control_[src & 7]; }

uint16_t I80186Peripherals::request_bits() const {
    uint16_t r = latched_;
    if (timer_requests_) r |= 1u << kTimer;
    for (unsigned n = 0; n < 4; ++n) {
        const unsigned src = kInt0 + n;
        if ((source_control(src) & 0x10) && pin_[n]) r |= uint16_t(1u << src);   // level-triggered
    }
    return r;
}

int I80186Peripherals::pending_source() const {
    // Nesting: a request interrupts only sources of lower priority (higher
    // number) than every one in service. Special fully nested mode (INT0/
    // INT1, bit 6) also lets a source interrupt itself, for a cascaded
    // slave with a request of its own.
    unsigned in_service_level = 8;
    for (unsigned src : kSources)
        if (in_service_ & (1u << src)) in_service_level = std::min(in_service_level, source_priority(src));
    const uint16_t req = request_bits();
    int best = -1;
    unsigned best_level = 8;
    for (unsigned src : kSources) {
        if (!(req & (1u << src)) || source_masked(src)) continue;
        const unsigned level = source_priority(src);
        if (level > priority_mask_) continue;
        const bool sfnm = (src == kInt0 || src == kInt1) && (source_control(src) & 0x40);
        if (in_service_ & (1u << src)) {
            if (!sfnm) continue;
        }
        if (level > in_service_level || (level == in_service_level && !sfnm)) continue;
        if (level < best_level) {
            best = int(src);
            best_level = level;
        }
    }
    return best;
}

uint8_t I80186Peripherals::vector_for(unsigned src) const {
    switch (src) {
    case kTimer:
        if (timer_requests_ & 1) return 0x08;
        if (timer_requests_ & 2) return 0x12;
        return 0x13;
    case kDma0: return 0x0A;
    case kDma1: return 0x0B;
    default: return uint8_t(0x0C + (src - kInt0));
    }
}

uint8_t I80186Peripherals::take(unsigned src) {
    uint8_t vec = vector_for(src);
    in_service_ |= uint16_t(1u << src);
    if (src == kTimer) {
        const unsigned t = vec == 0x08 ? 0 : vec == 0x12 ? 1 : 2;
        timer_requests_ = uint16_t(timer_requests_ & ~(1u << t));
        timer_in_service_ = uint8_t(t);
    } else {
        latched_ = uint16_t(latched_ & ~(1u << src));
    }
    return vec;
}

bool I80186Peripherals::intr_asserted() const { return pending_source() >= 0; }

uint8_t I80186Peripherals::acknowledge() {
    const int src = pending_source();
    if (src < 0) return 0x0F;   // spurious; the real part has no defined vector here
    const uint8_t vec = take(unsigned(src));
    if ((src == kInt0 || src == kInt1) && (source_control(unsigned(src)) & 0x20)) {
        if (InterruptController* slave = cascade_[src - kInt0]) return slave->acknowledge();
    }
    return vec;
}

void I80186Peripherals::eoi(uint16_t value) {
    if (value & 0x8000) {   // non-specific: the highest-priority source in service
        int best = -1;
        unsigned best_level = 8;
        for (unsigned src : kSources) {
            if (!(in_service_ & (1u << src))) continue;
            if (source_priority(src) < best_level) {
                best = int(src);
                best_level = source_priority(src);
            }
        }
        if (best >= 0) in_service_ = uint16_t(in_service_ & ~(1u << best));
        return;
    }
    const unsigned type = value & 0x1F;
    int src = -1;
    if (type == 0x08 || type == 0x12 || type == 0x13) src = kTimer;
    else if (type == 0x0A) src = kDma0;
    else if (type == 0x0B) src = kDma1;
    else if (type >= 0x0C && type <= 0x0F) src = int(kInt0 + (type - 0x0C));
    if (src >= 0) in_service_ = uint16_t(in_service_ & ~(1u << src));
    else LOG_DEBUG("i80186", "EOI for unknown type %02x", type);
}

void I80186Peripherals::set_int_line(unsigned n, bool level) {
    n &= 3;
    const bool rising = level && !pin_[n];
    pin_[n] = level;
    const unsigned src = kInt0 + n;
    if (rising && !(source_control(src) & 0x10)) latched_ |= uint16_t(1u << src);   // edge-triggered
}

// --- timers --------------------------------------------------------------

void I80186Peripherals::write_timer_control(unsigned n, uint16_t value) {
    Timer& t = timers_[n];
    // Timers 0/1: INT, MC, RTG, P, EXT, ALT, CONT (RIU is read-only).
    const uint16_t writable = n == 2 ? uint16_t(kInt | kMc | kCont) : uint16_t(kInt | 0x003F);
    uint16_t c = uint16_t((t.control & ~writable) | (value & writable));
    if (value & kInh) c = uint16_t((c & ~kEn) | (value & kEn));
    if (n == 2) c &= uint16_t(kEn | kInt | kMc | kCont);
    t.control = c;
}

void I80186Peripherals::timer_reached_max(unsigned n) {
    Timer& t = timers_[n];
    t.count = 0;
    t.control |= kMc;
    if (t.control & kInt) timer_requests_ |= uint16_t(1u << n);
    bool end_of_cycle = true;
    if (n < 2 && (t.control & kAlt)) {
        end_of_cycle = (t.control & kRiu) != 0;   // a full cycle ends after count B
        t.control ^= kRiu;
    }
    if (end_of_cycle && !(t.control & kCont)) t.control &= uint16_t(~kEn);
    // Timer 2 as the prescaler of timers 0/1.
    if (n == 2) {
        for (unsigned k = 0; k < 2; ++k) {
            const Timer& o = timers_[k];
            if ((o.control & kEn) && (o.control & kP) && !(o.control & kExt)) count_timer(k, 1);
        }
    }
}

void I80186Peripherals::count_timer(unsigned n, uint64_t increments) {
    Timer& t = timers_[n];
    while (increments && (t.control & kEn)) {
        const uint16_t max = (n < 2 && (t.control & kAlt) && (t.control & kRiu)) ? t.max_b : t.max_a;
        const uint32_t limit = max ? max : 0x10000;   // a maximum count of 0 means 65536
        const uint32_t to_go = limit > t.count ? limit - t.count : 1;
        if (increments < to_go) {
            t.count = uint16_t(t.count + increments);
            return;
        }
        increments -= to_go;
        timer_reached_max(n);
    }
}

void I80186Peripherals::tick(uint64_t cycles) {
    // Timers count on every fourth CPU clock (documented).
    clock_residue_ += cycles;
    const uint64_t inc = clock_residue_ / 4;
    clock_residue_ %= 4;
    if (!inc) return;
    count_timer(2, inc);
    for (unsigned n = 0; n < 2; ++n) {
        const uint16_t c = timers_[n].control;
        if ((c & kEn) && !(c & kP) && !(c & kExt)) count_timer(n, inc);
    }
}

// --- registers -------------------------------------------------------------

uint16_t I80186Peripherals::read_reg(unsigned off) {
    off &= 0xFE;
    if (off == kPoll) {
        const int src = pending_source();
        return src < 0 ? 0 : uint16_t(0x8000 | take(unsigned(src)));
    }
    if (off == kPollStatus) {
        const int src = pending_source();
        return src < 0 ? 0 : uint16_t(0x8000 | vector_for(unsigned(src)));
    }
    if (off == kEoi) return 0;
    if (off == kImask) {
        uint16_t m = 0;
        for (unsigned src : kSources)
            if (source_masked(src)) m |= uint16_t(1u << src);
        return m;
    }
    if (off == kPriMask) return priority_mask_;
    if (off == kInService) return in_service_;
    if (off == kRequest) return request_bits();
    if (off == kIntStatus) return timer_requests_;
    if (off >= kTimerCtl && off <= kInt3Ctl) {
        for (unsigned src : kSources)
            if (control_offset(src) == off) return control_[src];
    }
    if (off >= kTimer0 && off < kTimerEnd) {
        const unsigned n = (off - kTimer0) / 8, r = (off - kTimer0) % 8;
        const Timer& t = timers_[n];
        if (n == 2) return r == 0 ? t.count : r == 2 ? t.max_a : r == 6 ? t.control : 0;
        return r == 0 ? t.count : r == 2 ? t.max_a : r == 4 ? t.max_b : t.control;
    }
    if (off >= kUmcs && off <= kMpcs) return chip_select_[(off - kUmcs) / 2];
    if (off >= kDma0Regs && off < kDmaEnd) {
        const unsigned ch = off >= kDma1Regs, r = (off - (ch ? kDma1Regs : kDma0Regs)) / 2;
        const DmaChannel& d = dma_[ch];
        const uint16_t v[] = {d.src_lo, d.src_hi, d.dst_lo, d.dst_hi, d.count, d.control};
        return r < 6 ? v[r] : 0;
    }
    if (off == kRelocation) return relocation_;
    return 0;
}

void I80186Peripherals::write_reg(unsigned off, uint16_t value) {
    off &= 0xFE;
    if (off == kEoi) return eoi(value);
    if (off == kImask) {
        for (unsigned src : kSources)
            control_[src] = uint16_t((control_[src] & ~0x08) | ((value >> src) & 1 ? 0x08 : 0));
        return;
    }
    if (off == kPriMask) {
        priority_mask_ = value & 7;
        return;
    }
    if (off == kInService) {
        in_service_ = value & 0xFD;
        return;
    }
    if (off == kRequest) {   // only the DMA request bits are writable
        latched_ = uint16_t((latched_ & ~0x0C) | (value & 0x0C));
        return;
    }
    if (off == kIntStatus) {
        timer_requests_ = value & 7;
        return;
    }
    if (off >= kTimerCtl && off <= kInt3Ctl) {
        for (unsigned src : kSources) {
            if (control_offset(src) != off) continue;
            // Timer and DMA: priority and mask; INT0/INT1 add LVL, C and SFNM;
            // INT2/INT3 add LVL.
            const uint16_t bits = src < kInt0 ? 0x0F : src < kInt2 ? 0x7F : 0x1F;
            control_[src] = value & bits;
        }
        return;
    }
    if (off >= kTimer0 && off < kTimerEnd) {
        const unsigned n = (off - kTimer0) / 8, r = (off - kTimer0) % 8;
        Timer& t = timers_[n];
        if (r == 0) t.count = value;
        else if (r == 2) t.max_a = value;
        else if (r == 4 && n < 2) t.max_b = value;
        else if (r == 6) write_timer_control(n, value);
        return;
    }
    if (off >= kUmcs && off <= kMpcs) {
        chip_select_[(off - kUmcs) / 2] = value;
        return;
    }
    if (off >= kDma0Regs && off < kDmaEnd) {
        const unsigned ch = off >= kDma1Regs, r = (off - (ch ? kDma1Regs : kDma0Regs)) / 2;
        DmaChannel& d = dma_[ch];
        uint16_t* v[] = {&d.src_lo, &d.src_hi, &d.dst_lo, &d.dst_hi, &d.count, &d.control};
        if (r < 6) *v[r] = value;
        // CON bit 1 (ST) takes effect only with bit 2 (CHG) set.
        if (r == 5 && (value & 0x0006) == 0x0006 && !dma_logged_) {
            LOG_WARN("i80186", "DMA channel %u started (control %04x): DMA transfers are not modelled", ch, value);
            dma_logged_ = true;
        }
        return;
    }
    if (off == kRelocation) {
        relocation_ = value & 0x7FFF;
        remap();
        return;
    }
    LOG_DEBUG("i80186", "write to unassigned PCB offset %02x <- %04x", off, value);
}

// Byte accesses: the register's half for a read; a write replaces that half.
uint8_t I80186Peripherals::io_read8(uint16_t port) {
    const uint16_t v = read_reg(port & 0xFE);
    return uint8_t(port & 1 ? v >> 8 : v);
}

void I80186Peripherals::io_write8(uint16_t port, uint8_t value) { mmio_write8(port & 0xFF, value); }

uint8_t I80186Peripherals::mmio_read8(uint32_t offset) { return io_read8(uint16_t(offset & 0xFF)); }

void I80186Peripherals::mmio_write8(uint32_t offset, uint8_t value) {
    const unsigned off = offset & 0xFE;
    // The current value without side effects (POLL would acknowledge).
    const uint16_t cur = off == kPoll ? 0 : read_reg(off);
    write_reg(off, offset & 1 ? uint16_t((cur & 0x00FF) | (value << 8)) : uint16_t((cur & 0xFF00) | value));
}

uint8_t I80186Peripherals::peek8(uint32_t offset) {
    const unsigned off = offset & 0xFE;
    const uint16_t v = off == kPoll ? read_reg(kPollStatus) : read_reg(off);
    return uint8_t(offset & 1 ? v >> 8 : v);
}

std::string I80186Peripherals::describe() const {
    auto& self = const_cast<I80186Peripherals&>(*this);
    std::string s = format("PCB at %s %05x (relocation %04x)\n", in_memory() ? "memory" : "I/O", base(), relocation_);
    s += format("interrupts: mask %02x  priority mask %u  in service %02x  requests %02x  timers %x\n",
                self.read_reg(kImask), priority_mask_, in_service_, request_bits(), timer_requests_);
    const char* names[] = {"timers", "", "DMA0", "DMA1", "INT0", "INT1", "INT2", "INT3"};
    for (unsigned src : kSources) {
        const uint16_t c = control_[src];
        s += format("  %-6s pri %u%s%s%s%s  pin %s\n", names[src], c & 7, c & 8 ? " masked" : "",
                    src >= kInt0 && (c & 0x10) ? " level" : "", (src == kInt0 || src == kInt1) && (c & 0x20) ? " cascade" : "",
                    (src == kInt0 || src == kInt1) && (c & 0x40) ? " sfnm" : "",
                    src >= kInt0 ? (pin_[src - kInt0] ? "high" : "low") : "-");
    }
    for (unsigned n = 0; n < 3; ++n) {
        const Timer& t = timers_[n];
        s += format("timer %u: count %04x max A %04x", n, t.count, t.max_a);
        if (n < 2) s += format(" B %04x", t.max_b);
        s += format(" control %04x%s\n", t.control, t.control & kEn ? " running" : "");
    }
    const uint16_t umcs = chip_select_[0], lmcs = chip_select_[1], pacs = chip_select_[2], mmcs = chip_select_[3],
                   mpcs = chip_select_[4];
    s += format("chip selects: UMCS %04x LMCS %04x PACS %04x MMCS %04x MPCS %04x\n", umcs, lmcs, pacs, mmcs, mpcs);
    s += format("  UCS %05x-FFFFF\n", (uint32_t(umcs & 0xFFC0) << 4) & 0xFFFFF);
    if (lmcs) s += format("  LCS 00000-%05x\n", ((uint32_t(lmcs & 0x3FC0) << 4) | 0x3FF) & 0xFFFFF);
    unsigned block = 0;
    for (unsigned b = 0; b < 7; ++b)
        if (mpcs & (0x100u << b)) block = 0x2000u << b;
    if (block) {
        const uint32_t mbase = uint32_t(mmcs & 0xFE00) << 4;
        s += format("  MCS0-3 %05x-%05x (4 x %u KiB)\n", mbase, mbase + block - 1, block / 4 / 1024);
    }
    if (pacs) {
        const bool mem = (mpcs & 0x40) != 0;
        const uint32_t pbase = mem ? (uint32_t(pacs & 0xFFC0) << 4) : (uint32_t(pacs & 0xFFC0) << 4) & 0xFFFF;
        s += format("  PCS0-%c at %s %05x, 128 bytes each\n", mpcs & 0x80 ? '6' : '4', mem ? "memory" : "I/O", pbase);
    }
    for (unsigned ch = 0; ch < 2; ++ch) {
        const DmaChannel& d = dma_[ch];
        s += format("DMA%u: src %04x:%04x dst %04x:%04x count %04x control %04x\n", ch, d.src_hi, d.src_lo, d.dst_hi,
                    d.dst_lo, d.count, d.control);
    }
    return s;
}

} // namespace em34xx
