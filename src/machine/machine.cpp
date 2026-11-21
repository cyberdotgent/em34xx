#include "machine/machine.hpp"

#include "machine/model_info.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>

namespace tn34xx {

const char* stop_reason_name(StopEvent::Reason r) {
    switch (r) {
    case StopEvent::Reason::None: return "running";
    case StopEvent::Reason::User: return "stopped";
    case StopEvent::Reason::Breakpoint: return "breakpoint";
    case StopEvent::Reason::Watchpoint: return "watchpoint";
    case StopEvent::Reason::BusFault: return "bus fault";
    case StopEvent::Reason::StepDone: return "step";
    case StopEvent::Reason::Reset: return "reset";
    }
    return "?";
}

Machine::Machine(const ModelInfo& model)
    : model_(model),
      bus_(model.address_bits),
      cpu_(make_cpu(bus_, model.cpu)),
      bp_bitmap_((size_t(bus_.address_mask()) + 1) / 8, 0),
      twinax_(std::make_unique<NullTwinaxHost>()) {}

Machine::~Machine() = default;

Device* Machine::find_device(const std::string& name) const {
    for (const auto& d : devices_)
        if (d->name() == name) return d.get();
    return nullptr;
}

void Machine::reset() {
    bus_.reset_devices();
    bus_.clear_stop();
    for (auto& d : devices_) d->reset();
    cpu_->reset();
    display_dirty_ = true;
}

bool Machine::take_display_change() {
    const uint64_t writes = bus_.observed_writes();
    const uint64_t phase = display_ ? display_->animation_phase() : 0;
    const bool changed = display_dirty_ || writes != seen_display_writes_ || phase != seen_animation_phase_;
    display_dirty_ = false;
    seen_display_writes_ = writes;
    seen_animation_phase_ = phase;
    return changed;
}

bool Machine::breakpoint_hit(uint32_t pc, int& id) {
    if (!(bp_bitmap_[pc >> 3] & (1u << (pc & 7)))) return false;
    for (const Breakpoint& b : breakpoints_) {
        if (b.address == pc) { id = b.id; return true; }
    }
    return false;
}

StopEvent Machine::run(uint64_t max_instructions, const std::atomic<bool>& user_stop,
                       bool skip_initial_breakpoint) {
    StopEvent ev;
    const bool have_devices = !devices_.empty();
    for (uint64_t n = 0; n < max_instructions; ++n) {
        const uint32_t pc = cpu_->pc_linear();
        if (user_stop.load(std::memory_order_relaxed)) {
            ev.reason = StopEvent::Reason::User;
            ev.pc = pc;
            return ev;
        }
        int bp_id = 0;
        if ((n > 0 || !skip_initial_breakpoint) && !breakpoints_.empty() && breakpoint_hit(pc, bp_id)) {
            ev.reason = StopEvent::Reason::Breakpoint;
            ev.pc = pc;
            ev.id = bp_id;
            // Temporary breakpoints are removed when hit.
            auto it = std::find_if(breakpoints_.begin(), breakpoints_.end(),
                                   [&](const Breakpoint& b) { return b.id == bp_id; });
            if (it != breakpoints_.end() && it->temporary) remove_breakpoint(bp_id);
            return ev;
        }

        if (trace_hook) trace_hook(*this);
        bus_.set_pc_context(pc);
        if (InterruptController* ic = bus_.interrupt_controller()) cpu_->set_intr(ic->intr_asserted());
        const uint32_t cycles = cpu_->step();
        if (have_devices)
            for (auto& d : devices_) d->tick(cycles);

        if (bus_.stop_pending()) {
            const BusStop s = bus_.stop_info();
            bus_.clear_stop();
            ev.pc = cpu_->pc_linear();
            ev.detail = format("%s (instruction at %06x)", s.describe().c_str(), pc);
            if (s.cause == BusStop::Cause::Watchpoint) {
                ev.reason = StopEvent::Reason::Watchpoint;
                ev.id = s.watch_id;
                ev.data_address = s.address;
                ev.data_write = s.write;
            } else {
                ev.reason = StopEvent::Reason::BusFault;
            }
            return ev;
        }
    }
    ev.reason = StopEvent::Reason::None;
    ev.pc = cpu_->pc_linear();
    return ev;
}

int Machine::add_breakpoint(uint32_t linear, bool temporary) {
    linear &= bus_.address_mask();
    Breakpoint b;
    b.id = next_bp_id_++;
    b.address = linear;
    b.temporary = temporary;
    breakpoints_.push_back(b);
    bp_bitmap_[linear >> 3] = uint8_t(bp_bitmap_[linear >> 3] | (1u << (linear & 7)));
    return b.id;
}

bool Machine::remove_breakpoint(int id) {
    auto it = std::find_if(breakpoints_.begin(), breakpoints_.end(), [&](const Breakpoint& b) { return b.id == id; });
    if (it == breakpoints_.end()) return false;
    const uint32_t addr = it->address;
    breakpoints_.erase(it);
    const bool still = std::any_of(breakpoints_.begin(), breakpoints_.end(),
                                   [&](const Breakpoint& b) { return b.address == addr; });
    if (!still) bp_bitmap_[addr >> 3] = uint8_t(bp_bitmap_[addr >> 3] & ~(1u << (addr & 7)));
    return true;
}

bool Machine::remove_breakpoint_at(uint32_t linear) {
    bool any = false;
    for (;;) {
        auto it = std::find_if(breakpoints_.begin(), breakpoints_.end(),
                               [&](const Breakpoint& b) { return b.address == linear; });
        if (it == breakpoints_.end()) break;
        remove_breakpoint(it->id);
        any = true;
    }
    return any;
}

void Machine::clear_breakpoints() {
    breakpoints_.clear();
    std::fill(bp_bitmap_.begin(), bp_bitmap_.end(), 0);
}

void Machine::render(Framebuffer& fb) {
    (display_ ? display_ : &fallback_display_)->render(fb);
}

void Machine::key_event(const HostKeyEvent& ev) {
    (keyboard_ ? keyboard_ : &fallback_keyboard_)->host_key(ev);
}

bool Machine::terminal_key(const std::string& function, bool pressed) {
    return keyboard_ && keyboard_->terminal_key(function, pressed);
}

bool Machine::set_keymap(const std::string& spec, std::string& error) {
    if (!keyboard_) {
        error = "this model has no keyboard";
        return false;
    }
    if (!keyboard_->set_host_keymap(spec, options_.shift_fkeys, error)) return false;
    options_.keymap = spec;
    return true;
}

// ---------------------------------------------------------------------------

void PlaceholderDisplay::render(Framebuffer& fb) {
    if (fb.width != width_ || fb.height != height_) fb.resize(width_, height_);
    // A dim checkerboard: clearly "no display model", not a blank screen.
    for (int y = 0; y < height_; ++y) {
        uint32_t* row = fb.row(y);
        for (int x = 0; x < width_; ++x) {
            const bool check = ((x / 16) + (y / 16)) & 1;
            row[x] = check ? 0xFF203040u : 0xFF101820u;
        }
    }
}

void LoggingKeyboard::host_key(const HostKeyEvent& ev) {
    LOG_DEBUG("kbd", "host key %s (usage %u) %s; no keyboard hardware modelled", ev.name.c_str(),
              ev.hid_usage, ev.pressed ? "down" : "up");
}

} // namespace tn34xx
