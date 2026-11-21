// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices/ibm3477/twinax_engine.hpp"

#include "bus/bus.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <stdexcept>

namespace em34xx {

// ---------------------------------------------------------------------------
// Engine

TwinaxEngine::TwinaxEngine(Bus& bus, unsigned channels, uint64_t clock_hz, TwinaxEngineLayout layout)
    : Device("twinax"), bus_(bus), layout_(layout), ch_(channels), clock_hz_(clock_hz ? clock_hz : 8000000),
      bit_cycles_(clock_hz_ / 1000000 ? clock_hz_ / 1000000 : 1) {
    if (channels == 0) throw std::invalid_argument("twinax engine needs at least one channel");
}

uint64_t TwinaxEngine::message_cycles(size_t frames) const {
    // Bit sync and code-violation preamble (about 8 bit times), then 16 bit
    // cells per frame at 1 Mbit/s.
    return bit_cycles_ * (8 + 16 * uint64_t(frames));
}

uint8_t TwinaxEngine::ram(uint32_t a) const { return bus_.peek8(a & 0xFFFF); }
void TwinaxEngine::set_ram(uint32_t a, uint8_t v) { bus_.poke8(a & 0xFFFF, v); }

void TwinaxEngine::raise(unsigned ch, uint8_t bits) {
    if (raise_) raise_(ch, bits);
}

uint8_t TwinaxEngine::pending(unsigned ch) const { return pending_ ? uint8_t(pending_(ch) & 0x7F) : 0; }

void TwinaxEngine::write_control(unsigned ch, uint8_t v) {
    Channel& c = ch_.at(ch);
    if (v & 0x80) c.busy = false;
    if (v & 0x10) c.new_byte2 = true;
}

uint8_t TwinaxEngine::read_control(unsigned ch) const {
    // Bit 3 would show a program still running; programs run to completion
    // between CPU accesses here.
    return ch_.at(ch).busy ? 0x80 : 0x00;
}

void TwinaxEngine::reset() {
    for (Channel& c : ch_) c = Channel{};
    reg_.fill(0);
    running_ = false;
    wrap_ = false;
    latch_ = 0;
    now_ = 0;
    line_ = LineState::Idle;
    line_until_ = 0;
    next_host_check_ = 0;
    in_flight_.clear();
    response_.clear();
}

TwinaxMessage TwinaxEngine::poll_response(unsigned ch, uint8_t cmd) {
    Channel& c = ch_[ch];
    if (cmd & 0x40) c.lpe = false;   // poll modifier: reset line parity error
    if (c.new_byte2) {
        c.level = !c.level;
        c.new_byte2 = false;
    }
    const bool busy = c.busy || pending(ch) != 0;
    const uint8_t status = uint8_t((busy ? 0x80 : 0) | (c.lpe ? 0x40 : 0) | (c.toggle ? 0x20 : 0) |
                                   (reg_[6] & 0x1E) | (c.level ? 0x01 : 0));
    c.toggle = !c.toggle;
    ++c.polls;
    TwinaxMessage resp;
    if (reg_[5] & 0x0C) {
        resp.push_back(TwinaxFrame{status, c.address});
        resp.push_back(TwinaxFrame{reg_[(7 + ch) & 0x0F], 7});
    } else {
        resp.push_back(TwinaxFrame{status, 7});
    }
    raise(ch, kEvent);
    return resp;
}

TwinaxMessage TwinaxEngine::run_program(unsigned ch, const TwinaxMessage& msg, uint8_t cls) {
    Channel& c = ch_[ch];
    const unsigned prog = cls >> 6;
    const uint32_t base = layout_.programs + layout_.program_stride * ch + 32 * prog;
    uint8_t acc = 0;
    bool queue_mode = false;
    TwinaxMessage resp;
    for (unsigned pc = 0; pc < 32; ++pc) {
        const uint8_t op = ram(base + pc);
        const unsigned lo = op & 0x0F;
        switch (op >> 4) {
        case 0x2: reg_[lo] = acc; continue;
        case 0x3: set_ram(desc(ch, lo), acc); continue;
        case 0x4: acc = reg_[lo]; continue;
        case 0x5: acc = ram(desc(ch, lo)); continue;
        case 0x8:
        case 0x9:
            if ((op & 0x08) && acc == 0) {   // activate not armed
                raise(ch, kInvalidActivate);
                return resp;
            }
            if (op & 0x02) queue_mode = true;
            if (op & 0x10) raise(ch, kActivateDone);
            if (op & 0x04) return resp;   // end
            continue;
        case 0xA: {
            uint16_t addr = uint16_t(reg_[0] | (reg_[1] << 8));
            uint16_t count = uint16_t(reg_[2] | (reg_[3] << 8));
            if (op & 0x02) {   // line -> RAM
                bool overrun = false;
                for (size_t i = queue_mode ? 0 : 1; i < msg.size(); ++i) {
                    if (count == 0) {
                        overrun = true;
                        break;
                    }
                    set_ram(addr++, msg[i].data);
                    --count;
                }
                // The receiver reports the end of the message (the frame
                // addressed 7, or the only frame) whatever the mover's mode:
                // after an Activate Write it is this status, not activate
                // complete, that has the firmware reset its queue and drop
                // busy.
                if (overrun) raise(ch, kOverrun);
                if (queue_mode) {
                    raise(ch, kQueued | kEndOfMessage);
                    ++c.queued;
                } else {
                    raise(ch, kEndOfMessage);
                    ++c.activates;
                }
            }
            if (op & 0x01) {   // RAM -> line
                for (; count; --count) resp.push_back(TwinaxFrame{ram(addr++), c.address});
                if (!resp.empty()) resp.back().address = 7;
                ++c.activates;
            }
            reg_[0] = uint8_t(addr);
            reg_[1] = uint8_t(addr >> 8);
            reg_[2] = uint8_t(count);
            reg_[3] = uint8_t(count >> 8);
            c.busy = true;
            continue;
        }
        case 0xC:
            // Start the response. For the poll class it is the status
            // response; otherwise the frames come from the transmit mover.
            if (prog == 2) resp = poll_response(ch, msg.front().data);
            continue;
        default:
            if (!warned_opcode_) {
                LOG_WARN("twinax", "channel %u program %u: unknown opcode %02X at +%u; program stopped", ch, prog,
                         op, pc);
                warned_opcode_ = true;
            }
            return resp;
        }
    }
    return resp;
}

TwinaxMessage TwinaxEngine::receive(const TwinaxMessage& msg, bool bad_parity) {
    if (msg.empty()) return {};
    const uint8_t station = msg.front().address;
    for (unsigned ch = 0; ch < ch_.size(); ++ch) {
        Channel& c = ch_[ch];
        if (!c.enabled || c.address != station) continue;
        if (bad_parity) {
            c.lpe = true;
            raise(ch, kLineError);
            return {};
        }
        const uint8_t cls = ram(layout_.class_table + (msg.front().data & 0x1F));
        // After a parity error only polls are accepted until one resets it.
        if (c.lpe && (cls >> 6) != 2) return {};
        return run_program(ch, msg, cls);
    }
    return {};
}

void TwinaxEngine::inject(const TwinaxMessage& msg, bool bad_parity) {
    for (const TwinaxFrame& f : msg) latch_ = f.data;
    const TwinaxMessage resp = receive(msg, bad_parity);
    for (const TwinaxFrame& f : resp) latch_ = f.data;
    LOG_DEBUG("twinax", "loopback: %s%s -> %s", format_twinax_message(msg).c_str(),
              bad_parity ? " (bad parity)" : "", resp.empty() ? "no response" : format_twinax_message(resp).c_str());
}

void TwinaxEngine::tick(uint64_t cycles) {
    now_ += cycles;
    switch (line_) {
    case LineState::Idle: {
        if (now_ < next_host_check_) return;
        next_host_check_ = now_ + bit_cycles_ * 20;
        // The controller is an independent device on the cable: its clock
        // runs and it transmits whether or not this terminal's receiver is
        // enabled. A held receiver (or one switched to internal loopback)
        // simply does not answer; see the Receiving state.
        TwinaxHost* h = host();
        if (!h) return;
        const uint64_t ns = (now_ / clock_hz_) * 1000000000ull + (now_ % clock_hz_) * 1000000000ull / clock_hz_;
        h->advance(ns);
        TwinaxMessage m;
        if (!h->to_station(m)) return;
        if (m.empty()) {
            h->no_response();
            return;
        }
        ++host_messages_;
        in_flight_ = std::move(m);
        line_ = LineState::Receiving;
        line_until_ = now_ + message_cycles(in_flight_.size());
        return;
    }
    case LineState::Receiving: {
        if (now_ < line_until_) return;
        response_ = (running_ && !wrap_) ? receive(in_flight_, false) : TwinaxMessage{};
        LOG_DEBUG("twinax", "line: %s -> %s", format_twinax_message(in_flight_).c_str(),
                  response_.empty() ? "no response" : format_twinax_message(response_).c_str());
        if (response_.empty()) {
            if (TwinaxHost* h = host()) h->no_response();
            line_ = LineState::Idle;
        } else {
            line_ = LineState::Responding;
            line_until_ = now_ + turnaround_cycles() + message_cycles(response_.size());
        }
        return;
    }
    case LineState::Responding:
        if (now_ < line_until_) return;
        ++host_responses_;
        if (TwinaxHost* h = host()) h->from_station(response_);
        line_ = LineState::Idle;
        return;
    }
}

std::string TwinaxEngine::describe() const {
    static const char* const kLine[] = {"idle", "receiving", "responding"};
    std::string t = format("  engine %s%s, line %s, %llu message(s) from the controller, %llu response(s)\n",
                           running_ ? "running" : "held", wrap_ ? ", loopback" : "", kLine[int(line_)],
                           (unsigned long long)host_messages_, (unsigned long long)host_responses_);
    for (unsigned i = 0; i < ch_.size(); ++i) {
        const Channel& c = ch_[i];
        t += format("  channel %u: station %u %s, busy %d, lpe %d, level %d, polls %llu, queued %llu, "
                    "activates %llu\n",
                    i, c.address, c.enabled ? "enabled" : "disabled", c.busy, c.lpe, c.level,
                    (unsigned long long)c.polls, (unsigned long long)c.queued, (unsigned long long)c.activates);
    }
    t += format("  loopback latch %02X\n", latch_);
    return t;
}

// ---------------------------------------------------------------------------
// 3477 register view

uint8_t Ibm3477TwinaxRegisters::twinax_read(uint32_t off) {
    switch (off) {
    case 0x00:
    case 0x01: return eng_.read_control(off);
    case 0x15: return misc_;
    case 0x2B: return eng_.loopback_latch();
    default: return off < reg_.size() ? reg_[off] : 0xFF;
    }
}

void Ibm3477TwinaxRegisters::twinax_write(uint32_t off, uint8_t v) {
    if (off < reg_.size()) reg_[off] = v;
    switch (off) {
    case 0x00:
    case 0x01: eng_.write_control(off, v); return;
    case 0x08:   // station addresses: channel 0 bits 0-2, channel 1 bits 3-5
        eng_.set_station_address(0, v & 7);
        eng_.set_station_address(1, (v >> 3) & 7);
        return;
    case 0x09:   // bit 1/2 = channel 0/1 enable, bit 7 = engine run
        eng_.set_channel_enabled(0, (v & 0x02) != 0);
        eng_.set_channel_enabled(1, (v & 0x04) != 0);
        eng_.set_running((v & 0x80) != 0);
        return;
    case 0x15:
        // Bit 6 = loopback; bits 3/2 = path; bit 4 = force a parity error;
        // bit 7 = start the transmit DMA. In loopback, each write sends the
        // frame generator's message.
        misc_ = v;
        eng_.set_wrap((v & 0x40) != 0);
        if ((v & 0x40) && (v & 0x0C) && eng_.running()) transmit((v & 0x80) || (reg_[0x25] & 0x20), (v & 0x10) != 0);
        return;
    default: return;
    }
}

void Ibm3477TwinaxRegisters::transmit(bool dma, bool bad_parity) {
    TwinaxMessage msg;
    const uint8_t station = reg_[0x24] & 7;
    if (dma) {
        uint16_t addr = uint16_t(reg_[0x20] | (reg_[0x21] << 8));
        const unsigned count = unsigned(reg_[0x22] | (reg_[0x23] << 8));
        for (unsigned i = 0; i < count && i < 0x1000; ++i) msg.push_back(TwinaxFrame{eng_.dma_read(addr++), station});
        if (msg.size() > 1) msg.back().address = 7;
    } else {
        msg.push_back(TwinaxFrame{reg_[0x26], station});
        if (reg_[0x25] & 0x0C) msg.push_back(TwinaxFrame{reg_[0x27], 7});
    }
    if (!msg.empty()) eng_.inject(msg, bad_parity);
}

void Ibm3477TwinaxRegisters::reset() {
    reg_.fill(0);
    misc_ = 0;
}

std::string Ibm3477TwinaxRegisters::describe() const {
    return format("  station addresses %02X, channel control %02X, test %02X, frame regs %02X %02X %02X %02X %02X\n",
                  reg_[0x08], reg_[0x09], misc_, reg_[0x24], reg_[0x25], reg_[0x26], reg_[0x27], reg_[0x28]);
}

} // namespace em34xx
