// SPDX-License-Identifier: GPL-3.0-or-later
// The 3483's coax adapter (58100-5812F), the board's interrupt status
// registers and its memory engine. Behaviour from the firmware's use of
// the adapter (analysis notes: the adapter specification and POST's wrap
// test), labelled observed / hypothesis; the 3270 coax command set is
// documented.
//
// The adapter decodes and answers the controller's commands itself; the
// firmware never sends or receives a word in link mode [observed]. It
// keeps the answers in a register block in RAM, register n at 50300+2n
// [observed: the command tables the firmware copies to 0:9A00 name the
// registers that way]: 50308 poll response, 5030A display control
// register, 50310 status, 5031A terminal ID, 5031E EAB feature ID, 50320
// feature-6 ID, 50322 printer ID, 50328-2E extended ID. It tells the
// firmware what happened through 5810C:
//   15 counter-1 buffer written      11 RESET
//   14 LOAD CONTROL REGISTER         10 long operation done, counter 1
//   13 feature-6 START OPERATION      9 long operation done, counter 2
//   12 printer START OPERATION        8 POLL ACK
//    7 poll action (alarm, click)     6 periodic timer
// [observed: the runtime's handlers and the wrap test; which command
// raises 13/12/7 is a hypothesis].
//
// Buffers [observed: the firmware's accesses]: counter 1 (13 bits)
// addresses 0:C000 + 2 * a, character in the even byte and its extended
// attribute (EAB) in the odd one; counter 2 (12 bits) is the attached
// printer's, at 0:A000 + 2 * a [hypothesis for the base].
#include "hw/board_3483.hpp"

#include "bus/bus.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

namespace em34xx {

namespace {

constexpr uint32_t kDisplayBuffer = 0xC000, kPrinterBuffer = 0xA000;
constexpr uint16_t kDisplayCells = 0x2000, kPrinterCells = 0x1000;
constexpr uint32_t kRegisters = 0x300;   // from the block RAM base
// RAM copies of the command tables, plane A (opcode, register) [observed].
constexpr uint32_t kTableDisplay = 0x9A00, kTablePrinter = 0x9B00;
constexpr uint8_t kUndefined = 0x06;

bool odd_ones(uint32_t v) {
    bool odd = false;
    for (; v; v &= v - 1) odd = !odd;
    return odd;
}

} // namespace

Board3483::Coax::Coax(Board3483& board) : Device("coax-3483"), board_(board) { board_.coax_ = this; }

void Board3483::Coax::reset() {
    regs_.fill(0);
    addr_ = addr2_ = 0;
    rx_ = tx_ = 0;
    mask_ = eab_mask_ = mask2_ = 0;
    cmd_ = 0;
    data_words_ = 0;
    waiting_ = armed_ = alarm_ = click_ = undefined_ = false;
    eab_pending_ = 0;
    reply_.clear();
    busy_ = 0;
    timer_left_ = 0;
    board_.status1_ = board_.status2_ = 0;
    board_.update_irq();
}

// --- word formats -----------------------------------------------------------

// Register view of a coax word [observed: every word of POST's wrap test]:
// bits 7-0 = word bits 9-2, bit 14 = word bit 0 (command), bit 15 = word
// bit 1 (data parity; the poll response's flag), bit 13 = set when the
// word has an even number of ones. Bit 12 is a "last word" marker on
// transmit only.
uint16_t Board3483::Coax::to_register(CoaxWord w) {
    uint16_t r = uint16_t(w.bits >> 2 & 0xFF);
    if (w.bits & 2) r |= 0x8000;
    if (w.bits & 1) r |= 0x4000;
    if (!odd_ones(w.bits)) r |= 0x2000;
    return r;
}

CoaxWord Board3483::Coax::from_register(uint16_t r) {
    return CoaxWord{uint16_t((r & 0xFF) << 2 | (r >> 15 & 1) << 1 | (r >> 14 & 1))};
}

uint32_t Board3483::Coax::cell(unsigned which, uint16_t at) const {
    return which == 2 ? kPrinterBuffer + 2u * (at % kPrinterCells) : kDisplayBuffer + 2u * (at % kDisplayCells);
}

uint8_t Board3483::Coax::reg_byte(unsigned index) const {
    return board_.bus_.peek8(board_.layout_.block_ram + kRegisters + 2 * index);
}

void Board3483::Coax::reply_byte(uint8_t byte) { reply_.push_back(CoaxWord::data(byte)); }

void Board3483::Coax::reply_poll() {
    // The armed response from 50308: bits 7-0 = word bits 9-2, bit 8 = word
    // bit 1, bit 9 = word bit 0; 0000 answers as TT/AR [observed]. Without
    // an armed response, TT/AR [hypothesis].
    if (!armed_) {
        reply_.push_back(CoaxWord::turnaround());
        return;
    }
    const uint32_t regs = board_.layout_.block_ram + kRegisters;
    const uint16_t v = uint16_t(board_.bus_.peek8(regs + 8) | board_.bus_.peek8(regs + 9) << 8);
    reply_.push_back(CoaxWord{uint16_t((v & 0xFF) << 2 | (v >> 8 & 1) << 1 | (v >> 9 & 1))});
}

// A command the firmware has patched to "undefined" in the RAM tables
// (the printer's while it is not ready) [observed]. Before the first RESET
// the tables are not there yet and the fixed decoder decides.
bool Board3483::Coax::defined(uint8_t cmd) const {
    uint32_t entry;
    if (cmd < 0x20) entry = kTableDisplay + 4u * cmd;
    else if ((cmd & 0xE0) == 0xC0) entry = kTablePrinter + 4u * (cmd & 0x1F);
    else return true;
    return board_.bus_.peek8(entry) != kUndefined;
}

// --- the decoder ------------------------------------------------------------

void Board3483::Coax::begin(uint8_t cmd) {
    cmd_ = cmd;
    data_words_ = 0;
    waiting_ = false;
    eab_pending_ = 0;
    const uint8_t low = cmd & 0x1F;
    // POLL and POLL ACK on any device address [documented; observed in
    // the tables].
    if (low == 0x01) {
        const unsigned action = cmd >> 6;   // 01 click off, 10 alarm, 11 click on [documented]
        if (action) {
            if (action == 2) alarm_ = true;
            else click_ = action == 3;
            board_.raise1(0x0080);
        }
        reply_poll();
        return;
    }
    if (low == 0x11) {
        armed_ = false;            // the next response needs a new arming [hypothesis]
        board_.raise1(0x0100);
        return;
    }
    if (!defined(cmd)) {
        undefined_ = true;
        return;
    }
    const bool display = cmd < 0x20, printer = (cmd & 0xE0) == 0xC0;
    const unsigned which = printer ? 2 : 1;
    if (display || printer) {
        uint16_t& ctr = counter(which);
        const uint16_t cells = printer ? kPrinterCells : kDisplayCells;
        switch (low) {
        case 0x02:   // RESET (display only)
            if (!display) break;
            armed_ = false;
            board_.raise1(0x0800);
            return;
        case 0x03:   // READ DATA
            reply_byte(board_.bus_.peek8(cell(which, ctr)));
            ctr = uint16_t((ctr + 1) % cells);
            return;
        case 0x0B:   // READ MULTIPLE: to the next 32-cell boundary [hypothesis]
            do {
                reply_byte(board_.bus_.peek8(cell(which, ctr)));
                ctr = uint16_t((ctr + 1) % cells);
            } while (ctr % 32);
            return;
        case 0x05: reply_byte(uint8_t(ctr >> 8 & (printer ? 0x0F : 0x1F))); return;   // READ ADDRESS COUNTER HI
        case 0x15: reply_byte(uint8_t(ctr)); return;                                    // READ ADDRESS COUNTER LO
        case 0x07:   // READ EXTENDED ID: 4 bytes from 50328 [observed registers]
            if (!display) break;
            for (unsigned i = 0; i < 4; ++i) reply_byte(reg_byte(0x14 + i));
            return;
        case 0x09: reply_byte(reg_byte(display ? 0x0D : 0x11)); return;   // READ TERMINAL ID
        case 0x0D: reply_byte(reg_byte(display ? 0x08 : 0x0C)); return;   // READ STATUS
        case 0x08:   // START OPERATION: the printer's [observed table]; undefined on the display
            if (!printer) break;
            board_.raise1(0x1000);
            return;
        case 0x04: case 0x14: case 0x0C: case 0x06: case 0x0E: case 0x10: case 0x12: case 0x16: case 0x0A:
        case 0x1A:
            waiting_ = true;
            return;
        default: break;
        }
        undefined_ = true;
        return;
    }
    switch (cmd) {
    case 0x77: reply_byte(reg_byte(0x0F)); return;   // EAB READ FEATURE ID (79 colour, 78)
    case 0x7D: reply_byte(reg_byte(0x0A)); return;   // EAB READ STATUS
    case 0x73:                                       // EAB READ DATA
        reply_byte(board_.bus_.peek8(cell(1, addr_) + 1));
        addr_ = uint16_t((addr_ + 1) % kDisplayCells);
        return;
    case 0x7B:                                       // EAB READ MULTIPLE
        do {
            reply_byte(board_.bus_.peek8(cell(1, addr_) + 1));
            addr_ = uint16_t((addr_ + 1) % kDisplayCells);
        } while (addr_ % 32);
        return;
    case 0x76: case 0x7A: case 0x7C: waiting_ = true; return;   // EAB LOAD MASK, WRITE ALTERNATE, WRITE UNDER MASK
    case 0x65: case 0x67: reply_byte(reg_byte(0x10)); return;   // feature 6 READ FEATURE ID (66)
    case 0x6D: reply_byte(reg_byte(0x09)); reply_byte(reg_byte(0x0B)); return;   // feature 6 READ STATUS
    case 0x68: board_.raise1(0x2000); return;                   // feature 6 START OPERATION
    default: break;
    }
    // Anything else (other feature addresses, their READ FEATURE ID) is not
    // answered: the controller learns that the feature is absent.
    undefined_ = true;
}

void Board3483::Coax::data(uint8_t byte) {
    if (!waiting_) return;
    ++data_words_;
    Bus& bus = board_.bus_;
    const bool printer = (cmd_ & 0xE0) == 0xC0;
    const unsigned which = printer ? 2 : 1;
    const uint16_t cells = printer ? kPrinterCells : kDisplayCells;
    uint8_t& mask = printer ? mask2_ : mask_;
    uint16_t& ctr = counter(which);
    auto store = [&](uint16_t at, uint8_t v) {
        const uint32_t a = cell(which, at);
        bus.poke8(a, uint8_t((bus.peek8(a) & mask) | (v & ~mask)));
    };
    const uint16_t done_bit = printer ? 0x0200 : 0x0400;
    switch (cmd_ < 0x20 || printer ? (cmd_ & 0x1F) : cmd_) {
    case 0x04: ctr = uint16_t((ctr & 0x00FF) | (byte & (printer ? 0x0F : 0x1F)) << 8); break;   // LAC HI
    case 0x14: ctr = uint16_t((ctr & 0xFF00) | byte); break;                                       // LAC LO
    case 0x0C:   // WRITE DATA
        store(ctr, byte);
        ctr = uint16_t((ctr + 1) % cells);
        if (!printer) board_.raise1(0x8000);
        break;
    case 0x16: mask = byte; break;   // LOAD MASK: bits set are kept [observed: POST loads 00]
    case 0x06:   // CLEAR: the pattern from the counter to the end of the buffer [hypothesis for the range]
        for (uint32_t a = ctr; a < cells; ++a) store(uint16_t(a), byte);
        board_.raise1(uint16_t(done_bit | (printer ? 0 : 0x8000)));
        break;
    case 0x10:   // SEARCH FORWARD / BACKWARD: the next matching cell under the mask [hypothesis]
    case 0x12: {
        const bool forward = (cmd_ & 0x1F) == 0x10;
        for (uint16_t i = 1; i <= cells; ++i) {
            const uint16_t at = uint16_t(forward ? (ctr + i) % cells : (ctr + cells - i) % cells);
            if (((bus.peek8(cell(which, at)) ^ byte) & ~mask) == 0) {
                ctr = at;
                break;
            }
        }
        board_.raise1(done_bit);
        break;
    }
    case 0x0E: {   // INSERT BYTE: shift right up to a null cell, then store [hypothesis]
        uint16_t end = ctr;
        while (end + 1u < cells && bus.peek8(cell(which, end)) != 0) ++end;
        for (uint16_t a = end; a > ctr; --a) {
            bus.poke8(cell(which, a), bus.peek8(cell(which, uint16_t(a - 1))));
            bus.poke8(cell(which, a) + 1, bus.peek8(cell(which, uint16_t(a - 1)) + 1));
        }
        store(ctr, byte);
        board_.raise1(uint16_t(done_bit | (printer ? 0 : 0x8000)));
        break;
    }
    case 0x0A:   // LOAD CONTROL REGISTER: 5030A (display), 5030C (printer)
        bus.poke8(board_.layout_.block_ram + kRegisters + 2 * (printer ? 0x06 : 0x05), byte);
        if (!printer) board_.raise1(0x4000);
        break;
    case 0x1A: bus.poke8(board_.layout_.block_ram + kRegisters + 2 * 0x07, byte); break;   // LOAD SECONDARY CONTROL: 5030E
    case 0x76: eab_mask_ = byte; break;                         // EAB LOAD MASK
    case 0x7A:   // EAB WRITE ALTERNATE: character and EAB bytes in turn
        if (!eab_pending_) {
            eab_pending_ = 1;
            bus.poke8(cell(1, addr_), byte);
        } else {
            eab_pending_ = 0;
            bus.poke8(cell(1, addr_) + 1, byte);
            addr_ = uint16_t((addr_ + 1) % kDisplayCells);
            board_.raise1(0x8000);
        }
        break;
    case 0x7C: {   // EAB WRITE UNDER MASK
        const uint32_t a = cell(1, addr_) + 1;
        bus.poke8(a, uint8_t((bus.peek8(a) & eab_mask_) | (byte & ~eab_mask_)));
        addr_ = uint16_t((addr_ + 1) % kDisplayCells);
        board_.raise1(0x8000);
        break;
    }
    default: break;
    }
}

bool Board3483::Coax::execute(const CoaxMessage& msg, CoaxMessage& reply) {
    reply_.clear();
    undefined_ = false;
    waiting_ = false;
    for (const CoaxWord& w : msg) {
        if (w.is_command()) begin(w.byte());
        else data(w.byte());
        if (undefined_) break;
    }
    if (undefined_) return false;
    reply = reply_.empty() ? CoaxMessage{CoaxWord::turnaround()} : reply_;
    return true;
}

// POST's wrap modes: the firmware's own words go through the decoder one
// at a time; 58108 echoes the word and 5810A reads back the reply.
void Board3483::Coax::wrap_transmit(uint16_t reg_word) {
    rx_ = reg_word;
    const CoaxWord w = from_register(uint16_t(reg_word & ~0x1000));
    if (w.is_command()) {
        reply_.clear();
        undefined_ = false;
        begin(w.byte());
    } else {
        reply_.clear();
        data(w.byte());
    }
    regs_[0x0A >> 1] = to_register(reply_.empty() ? CoaxWord::turnaround() : reply_.back());
}

// --- line -----------------------------------------------------------------------

void Board3483::Coax::tick(uint64_t cycles) {
    const uint64_t clock = board_.frame_cycles_ * 60;
    ns_acc_ += cycles * 1000000000ull;
    emu_ns_ += ns_acc_ / clock;
    ns_acc_ %= clock;
    // Periodic timer, 5810C bit 6 [observed: blink, saver, hour meter and
    // key repeat count it; hypothesis: about 44 Hz, from the hour meter].
    const uint64_t period = clock / 44;
    if (timer_left_ <= cycles) {
        timer_left_ = period;
        board_.raise1(0x0040);
    } else {
        timer_left_ -= cycles;
    }
    if (busy_ > cycles) {
        busy_ -= cycles;
        return;
    }
    busy_ = 0;
    // On line: 58100 C000 after the controller's first RESET, 6060 before
    // it [observed]; not at 2000 (reset, POST) or 6000 (off line), nor in a
    // wrap mode [hypothesis for the bits].
    const uint16_t mode = regs_[0];
    const uint16_t nibble = mode & 0xF000;
    const bool on_line = (mode & 0x4000) && ((mode & 0x8000) || (mode & 0x0060)) && nibble != 0x7000 && nibble != 0xD000;
    CoaxHost* host = line_ ? line_() : nullptr;
    if (!host) return;
    host->advance(emu_ns_);
    CoaxMessage msg;
    if (!host->to_terminal(msg)) return;
    CoaxMessage reply;
    if (on_line && execute(msg, reply)) host->from_terminal(reply);
    else host->no_response();
    // A word takes about 5 us at 2.3587 Mbit/s (12 bit cells), plus the
    // turnaround [documented rate; hypothesis for the turnaround].
    const uint64_t us = 5u * (msg.size() + reply.size()) + 4u;
    busy_ = clock * us / 1000000;
}

// --- registers --------------------------------------------------------------

void Board3483::Coax::run_engine(uint16_t command) {
    // The ASIC's memory engine [observed: POST display test entries A1/A2,
    // C000:C5D8 and C000:C699]: 58118 = source/pointer, 5811A =
    // destination, 5811C = word count, started by 58110 bit 7, which reads
    // back as busy. Done at once here.
    //  - 00: copy words unchanged [observed: runtime copies 7000 -> 1000];
    //  - 08: copy, keeping the character byte and clearing the attribute
    //    byte (38A0 -> 00A0);
    //  - 30: search backward from 58118 for a field attribute (character
    //    byte C0-FF), leaving 58118 at it, or count cells back when there is
    //    none [observed: the runtime's field lookup, F000:BAAD, reads the
    //    cell at 58118 as the field attribute when its character is C0-FF
    //    and takes "start - 2 * count" as no field; POST's check, a CA
    //    among 20 cells, fits].
    // Bits 5-3 are the operation. Bit 6 is a flag the runtime sets during
    // the display test (state 92) and on request; the firmware reads the
    // results the same with it set, so it is taken to leave the data
    // unchanged [hypothesis: timing, e.g. waiting for blanking]. Bit 0 is
    // video on.
    Bus& bus = board_.bus_;
    const uint16_t op = command & 0x38;
    uint16_t src = regs_[0x18 >> 1], dst = regs_[0x1A >> 1];
    const uint16_t count = regs_[0x1C >> 1];
    if (op == 0x00) {
        for (uint16_t i = 0; i < count; ++i, src += 2, dst += 2) {
            bus.poke8(dst, bus.peek8(src));
            bus.poke8(uint32_t(dst) + 1, bus.peek8(uint32_t(src) + 1));
        }
    } else if (op == 0x08) {
        for (uint16_t i = 0; i < count; ++i, src += 2, dst += 2) {
            bus.poke8(dst, bus.peek8(src));
            bus.poke8(uint32_t(dst) + 1, 0);
        }
    } else if (op == 0x30) {
        for (uint16_t i = 0; i < count; ++i, src -= 2)
            if ((bus.peek8(src) & 0xC0) == 0xC0) break;
        regs_[0x18 >> 1] = src;
    } else {
        LOG_WARN("coax", "memory engine command %04x is not modelled (from %04x to %04x, %u words)", command, src,
                 dst, count);
    }
    regs_[0x10 >> 1] = uint16_t(command & ~0x0080);
}

uint16_t Board3483::Coax::read_word(unsigned off, bool side_effects) {
    (void)side_effects;
    off &= 0xFE;
    switch (off) {
    case 0x02:   // bit 7 alarm requested, bit 6 click on [observed use]
        return uint16_t((regs_[1] & ~0x00C0) | (alarm_ ? 0x80 : 0) | (click_ ? 0x40 : 0));
    case 0x04: return uint16_t(addr_ & 0x1FFF);
    case 0x06: return uint16_t(addr2_ & 0x0FFF);
    case 0x08: return rx_;
    case 0x0C: return board_.status1_;
    case 0x10: return uint16_t(regs_[off >> 1] & ~0x0080);   // engine busy: never [observed: polled without timeout]
    case 0x20: return board_.status2_;
    default: return regs_[off >> 1];
    }
}

void Board3483::Coax::write_word(unsigned off, uint16_t value) {
    off &= 0xFE;
    switch (off) {
    case 0x00: {
        regs_[0] = value;
        const uint16_t nibble = value & 0xF000;
        if ((value & 0x0100) && (nibble == 0x7000 || nibble == 0xD000)) wrap_transmit(tx_);   // bit 8: send 5810A
        return;
    }
    case 0x02:
        // Arming a poll response: 50308, then 58102 with bit 7 [observed].
        regs_[1] = value;
        if (value & 0x0080) armed_ = true;
        alarm_ = false;
        return;
    case 0x04: addr_ = uint16_t(value & 0x1FFF); return;
    case 0x06: addr2_ = uint16_t(value & 0x0FFF); return;
    case 0x08: rx_ = value; return;
    case 0x0A: tx_ = value; return;   // reads of +0A give the adapter's reply word
    case 0x0C:
        board_.status1_ = uint16_t(board_.status1_ & ~value);
        board_.update_irq();
        return;
    case 0x0E:
    case 0x22:
        regs_[off >> 1] = value;
        board_.update_irq();
        return;
    case 0x10:
        regs_[off >> 1] = value;
        if (value & 0x0080) run_engine(value);
        return;
    case 0x20:
        board_.status2_ = uint16_t(board_.status2_ & ~value);
        board_.update_irq();
        return;
    default: regs_[off >> 1] = value;
    }
}

uint16_t Board3483::Coax::mmio_read16(uint32_t off) { return read_word(off, true); }
void Board3483::Coax::mmio_write16(uint32_t off, uint16_t value) { write_word(off, value); }

uint8_t Board3483::Coax::mmio_read8(uint32_t off) {
    const uint16_t w = read_word(off, true);
    return uint8_t(off & 1 ? w >> 8 : w);
}

void Board3483::Coax::mmio_write8(uint32_t off, uint8_t value) {
    const uint16_t cur = read_word(off, false);
    write_word(off, off & 1 ? uint16_t((cur & 0x00FF) | (value << 8)) : uint16_t((cur & 0xFF00) | value));
}

uint8_t Board3483::Coax::peek8(uint32_t off) {
    const uint16_t w = read_word(off, false);
    return uint8_t(off & 1 ? w >> 8 : w);
}

std::string Board3483::Coax::describe() const {
    return format("coax 58100: control %04x, counters %04x/%04x, poll response %s, status %04x/%04x, enable %04x/%04x\n",
                  regs_[0], addr_, addr2_, armed_ ? "armed" : "idle", board_.status1_, board_.status2_, regs_[7],
                  regs_[0x11]);
}

} // namespace em34xx
