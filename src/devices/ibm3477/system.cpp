#include "devices/ibm3477/system.hpp"

#include "devices/ibm3477/keyboard_if.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

namespace tn34xx {

// ---------------------------------------------------------------------------
// Interrupt controller

uint8_t Ibm3477Interrupts::vector(Source s) {
    switch (s) {
    case Source::Timer: return 0x06;
    case Source::Twinax0Event: return 0x07;
    case Source::Twinax1Event: return 0x08;
    case Source::Twinax0Status: return 0x09;
    case Source::Twinax1Status: return 0x0A;
    case Source::Display: return 0x0C;
    case Source::Keyboard: return 0x0E;
    }
    return 0xFF;
}

const char* Ibm3477Interrupts::source_name(Source s) {
    switch (s) {
    case Source::Timer: return "timer";
    case Source::Twinax0Event: return "twinax0 event";
    case Source::Twinax1Event: return "twinax1 event";
    case Source::Twinax0Status: return "twinax0 status";
    case Source::Twinax1Status: return "twinax1 status";
    case Source::Display: return "display";
    case Source::Keyboard: return "keyboard";
    }
    return "?";
}

static uint16_t source_bits(Ibm3477Interrupts::Source s) {
    using S = Ibm3477Interrupts::Source;
    switch (s) {
    case S::Timer: return Ibm3477Interrupts::kTimerBit;
    case S::Twinax0Event: return Ibm3477Interrupts::kTwinax0EventBit;
    case S::Twinax1Event: return Ibm3477Interrupts::kTwinax1EventBit;
    case S::Twinax0Status: return Ibm3477Interrupts::kTwinax0StatusBits;
    case S::Twinax1Status: return Ibm3477Interrupts::kTwinax1StatusBits;
    default: return 0;
    }
}

void Ibm3477Interrupts::raise(Source s) {
    if (s == Source::Display) display_ = true;
    else if (s == Source::Keyboard) keyboard_ = true;
    else {
        const uint16_t bits = source_bits(s);
        req_ |= uint16_t(bits & -bits);   // lowest bit of the group
    }
}

void Ibm3477Interrupts::lower(Source s) {
    if (s == Source::Display) display_ = false;
    else if (s == Source::Keyboard) keyboard_ = false;
    else req_ = uint16_t(req_ & ~source_bits(s));
}

bool Ibm3477Interrupts::line(Source s) const {
    if (s == Source::Display) return display_;
    if (s == Source::Keyboard) return keyboard_;
    return (req_ & source_bits(s)) != 0;
}

bool Ibm3477Interrupts::highest(Source& out) const {
    const uint16_t active = uint16_t(req_ & mask_);
    for (int i = 0; i < kSourceCount; ++i) {
        const Source s = Source(i);
        bool pending;
        if (s == Source::Display) pending = display_;   // enable not identified: always on
        else if (s == Source::Keyboard) pending = keyboard_ && kbd_enable_;
        else pending = (active & source_bits(s)) != 0;
        if (pending) {
            out = s;
            return true;
        }
    }
    return false;
}

bool Ibm3477Interrupts::intr_asserted() const {
    Source s;
    return highest(s);
}

uint8_t Ibm3477Interrupts::acknowledge() {
    Source s;
    if (!highest(s)) {
        LOG_WARN("irq", "interrupt acknowledge with no source pending");
        return 0xFF;
    }
    ++delivered_[size_t(s)];
    return vector(s);
}

void Ibm3477Interrupts::reset() {
    req_ = 0;
    mask_ = 0;
    kbd_enable_ = false;
    display_ = false;
    keyboard_ = false;
}

std::string Ibm3477Interrupts::describe() const {
    std::string t = format("  irq: request %04X mask %04X, keyboard %s%s, display %s\n", req_, mask_,
                           keyboard_ ? "requesting" : "idle", kbd_enable_ ? "" : " (disabled)",
                           display_ ? "requesting" : "idle");
    t += "  delivered:";
    for (int i = 0; i < kSourceCount; ++i)
        t += format(" %02X=%llu", vector(Source(i)), (unsigned long long)delivered_[size_t(i)]);
    return t + "\n";
}

// ---------------------------------------------------------------------------
// Straps

uint8_t ibm3477_default_straps(DisplaySubtype d) {
    // Bits 3..1 carry the display type; only 010, 110 and 111 are accepted
    // (types 3, 4, 5) [observed]. The firmware reports the type in the Read
    // Info VPD as the model suffix FD0, FE0 and FW0 respectively, all with
    // the monochrome model ID 24 [observed]; the colour model (FC0, model ID
    // 50) is a type that no strap value selects in this ROM. So: paper-white
    // is FW0 (111) [hypothesis: W = white; type 5 also has its own cell
    // geometry and monitor-control setting]; green and amber are 110
    // [hypothesis]; colour has no strap code of its own and uses 111, the
    // closest tube, so the firmware identifies it as a monochrome FW0.
    return (d == DisplaySubtype::Color || d == DisplaySubtype::PaperWhite) ? 0x0E : 0x0C;
}

// ---------------------------------------------------------------------------
// Gate array

Ibm3477System::Ibm3477System(uint64_t clock_hz, uint8_t straps, std::unique_ptr<Ibm3477KeyboardInterface> kbd)
    : Device("system"), kbd_(std::move(kbd)), straps_(straps),
      timer_period_((clock_hz ? clock_hz : 8000000) / kTimerHz) {
    kbd_->connect(&ic_);
}

Ibm3477System::~Ibm3477System() { kbd_->connect(nullptr); }

bool Ibm3477System::is_twinax_register(uint32_t off) {
    return off <= 0x01 || off == 0x08 || off == 0x09 || (off >= 0x20 && off <= 0x2F);
}

void Ibm3477System::reset() {
    ic_.reset();
    latch_.fill(0);
    status_latch_ = 0;
    misc_ = 0;
    timer_acc_ = 0;
    kbd_->reset();
}

void Ibm3477System::tick(uint64_t cycles) {
    timer_acc_ += cycles;
    if (timer_acc_ >= timer_period_) {
        timer_acc_ -= timer_period_;
        ++timer_ticks_;
        ic_.raise(Ibm3477Interrupts::Source::Timer);
    }
    kbd_->tick(cycles);
}

uint8_t Ibm3477System::read(uint32_t off, bool side_effects) {
    // The link registers read without side effects, so the debugger sees them too.
    (void)side_effects;
    if (twinax_ && is_twinax_register(off)) return twinax_->twinax_read(off);
    switch (off) {
    case 0x02: return uint8_t(ic_.requests());
    case 0x03: return uint8_t(ic_.requests() >> 8);
    case 0x04: return uint8_t(ic_.mask());
    case 0x05: return uint8_t(ic_.mask() >> 8);
    case 0x0A: return straps_;
    case 0x10: return status_latch_;
    case 0x12: return kbd_->read(0);
    case 0x13: return kbd_->read(1);
    case 0x15: return misc_;
    default: return latch_[off];
    }
}

uint8_t Ibm3477System::mmio_read8(uint32_t off) { return read(off & 0xFF, true); }
uint8_t Ibm3477System::peek8(uint32_t off) { return read(off & 0xFF, false); }

void Ibm3477System::mmio_write8(uint32_t off, uint8_t v) {
    off &= 0xFF;
    if (twinax_ && is_twinax_register(off)) {
        twinax_->twinax_write(off, v);
        return;
    }
    switch (off) {
    case 0x02: ic_.acknowledge_bits(v); return;
    case 0x03: ic_.acknowledge_bits(uint16_t(v << 8)); return;
    case 0x04: ic_.set_mask(uint16_t((ic_.mask() & 0xFF00) | v)); return;
    case 0x05: ic_.set_mask(uint16_t((ic_.mask() & 0x00FF) | (v << 8))); return;
    case 0x07:
        latch_[off] = v;
        ic_.set_keyboard_enabled(v & 1);
        return;
    case 0x10:
        if ((v ^ status_latch_) & 0x80)
            LOG_DEBUG("sound", "alarm tone %s (setting %02X)", (v & 0x80) ? "on" : "off", v & 0x7F);
        status_latch_ = v;
        return;
    case 0x12: kbd_->write(0, v); return;
    case 0x13: kbd_->write(1, v); return;
    case 0x14: kbd_->write(2, v); return;
    case 0x15:
        if ((v ^ misc_) & 0x02) LOG_TRACE("sound", "speaker gate %s", (v & 0x02) ? "on" : "off");
        misc_ = v;
        // Bits 2-4, 6 and 7 control the Twinax engine's loopback test path.
        if (twinax_) twinax_->twinax_write(off, v);
        else if (v & 0x40) LOG_DEBUG("twinax", "loopback control %02X (no link device)", v);
        return;
    default:
        latch_[off] = v;
        return;
    }
}

std::string Ibm3477System::describe() const {
    std::string t = ic_.describe();
    t += format("  timer: %u Hz (every %llu clocks), %llu ticks\n", kTimerHz, (unsigned long long)timer_period_,
                (unsigned long long)timer_ticks_);
    t += format("  straps %02X, status latch %02X (tone %s), misc %02X (speaker %s)\n", straps_, status_latch_,
                tone_on() ? "on" : "off", misc_, speaker_gate() ? "on" : "off");
    t += kbd_->describe();
    t += format("  twinax: %s\n", twinax_ ? "link device attached" : "registers stubbed (no link device)");
    return t;
}

} // namespace tn34xx
