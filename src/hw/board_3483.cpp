// SPDX-License-Identifier: GPL-3.0-or-later
// See board_3483.hpp. Register roles are taken from the firmware's use
// (observed in code) unless marked hypothesis.
#include "hw/board_3483.hpp"

#include "bus/bus.hpp"
#include "hw/i80186_peripherals.hpp"
#include "input/set3_keyboard.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

namespace em34xx {

// --- board -------------------------------------------------------------------

Board3483::Board3483(Bus& bus, I80186Peripherals& cpu_io, uint64_t clock_hz, uint8_t monitor_sense)
    : bus_(bus), cpu_io_(cpu_io), display_(nullptr), coax_(nullptr), system_(nullptr),
      monitor_sense_(monitor_sense) {
    // 60 Hz frame with a short vertical sync [hypothesis]. POST measures
    // both phases in loop counts and accepts a window around this.
    frame_cycles_ = clock_hz / 60;
    active_cycles_ = frame_cycles_ - frame_cycles_ / 40;
    cpu_io_.set_cascade_controller(0, this);
}

void Board3483::advance(uint64_t cycles) {
    frame_pos_ += cycles;
    while (frame_pos_ >= frame_cycles_) {
        frame_pos_ -= frame_cycles_;
        ++frame_count_;
        // A frame with the video test on (5801A bit 5) and no output
        // selected (58021 bits 0-2 = 0) raises 5810C bit 5 [hypothesis
        // fitted to the display test: it expects the interrupt for that
        // case only].
        if (display_ && (display_->reg(0x1A) & 0x20) && ((display_->reg(0x20) >> 8) & 7) == 0) raise1(0x0020);
    }
}

void Board3483::raise1(uint16_t bits) {
    status1_ |= bits;
    update_irq();
}

void Board3483::raise2(uint16_t bits) {
    status2_ |= bits;
    update_irq();
}

// INT0 is the OR of the enabled status bits: 5810E enables 5810C bits and
// 58122 enables 58120 bits [hypothesis: both are written 0 at reset and
// set later, and POST polls the status registers with them 0].
bool Board3483::intr_asserted() const {
    const uint16_t en1 = coax_ ? coax_->reg(0x0E) : 0, en2 = coax_ ? coax_->reg(0x22) : 0;
    return (status1_ & en1) != 0 || (status2_ & en2) != 0;
}

void Board3483::update_irq() { cpu_io_.set_int_line(0, intr_asserted()); }

void Board3483::set_window(bool on) {
    if (on == window_mapped_ || !window_) return;
    if (on) bus_.map_mmio("window 40000", 0x40000, 0x8000, window_);
    else bus_.unmap_device_mmio(window_);
    window_mapped_ = on;
}

// --- 40000 write window --------------------------------------------------------

uint8_t* Board3483::Window::sram(uint32_t offset) {
    // The SRAM region itself, beneath the window (48000 is never covered).
    const Region* r = board_.bus_.region_at(0x48000);
    if (!r || r->kind != Region::Kind::Ram) return nullptr;
    const uint32_t a = 0x40000 + offset;
    if (a < r->base || a >= r->base + r->size) return nullptr;
    return const_cast<uint8_t*>(&r->storage[a - r->base]);
}

uint8_t Board3483::Window::mmio_read8(uint32_t offset) {
    const uint8_t* p = sram(offset);
    return p ? *p : 0xFF;
}

void Board3483::Window::mmio_write16(uint32_t offset, uint16_t value) {
    offset &= ~1u;
    uint8_t* lo = sram(offset);
    uint8_t* hi = sram(offset + 0x8000);
    if (lo) lo[0] = lo[1] = uint8_t(value);
    if (hi) hi[0] = hi[1] = uint8_t(value >> 8);
}

void Board3483::Window::mmio_write8(uint32_t offset, uint8_t value) {
    // A byte write: the even byte is the low (first-plane) half [hypothesis].
    if (offset & 1) {
        if (uint8_t* hi = sram((offset & ~1u) + 0x8000)) hi[0] = hi[1] = value;
    } else if (uint8_t* lo = sram(offset)) {
        lo[0] = lo[1] = value;
    }
}

// --- display controller (58000) ----------------------------------------------

namespace {

constexpr uint32_t kCgBase = 0x48000;        // base character set [observed: decompressed there]
constexpr uint32_t kCgExtension = 0x40000;   // its companion plane [observed]; dots 9+ [hypothesis]

} // namespace

Board3483::Display::Display(Board3483& board) : Device("display-3483"), board_(board) { board_.display_ = this; }

void Board3483::Display::reset() {
    regs_.fill(0);
    board_.set_window(false);
}

uint16_t Board3483::Display::read_word(unsigned off) const {
    off &= 0xFE;
    if (off == 0x20) {
        // 58020 (read): video drive sense [hypothesis]: the number of the
        // three outputs selected by 58021 bits 0-2 that carried a lit dot
        // while video was on (58110 bit 0) [observed: the display test
        // expects 3->2, 2->1, 4->1 over a test pattern, and 0 over a blank
        // screen or with video off]. A cell is taken as lit when it holds a
        // character other than null (00) or space (10) and its attribute
        // byte does not have bit 5 set [hypothesis: bit 5 marks a 3270 field
        // attribute, which shows as a blank; the blank test screen has 02/20
        // in its first and last columns].
        const uint16_t sel = uint16_t((regs_[0x20 >> 1] >> 8) & 7);
        const bool video = board_.coax_ && (board_.coax_->reg(0x10) & 1);
        unsigned n = 0;
        if (video && screen_has_ink())
            for (uint16_t b = sel; b; b &= uint16_t(b - 1)) ++n;
        return uint16_t((regs_[0x20 >> 1] & 0xFF00) | n);
    }
    if (off == 0x26) {
        // Bit 3: the internal service table (no option ROM). Bit 0: sync
        // polarity of the monitor, 1 = vertical sync reads as 1 in 5840E
        // bit 2 [observed use; value hypothesis].
        return uint16_t((regs_[off >> 1] & ~0x0009) | 0x0009);
    }
    return regs_[off >> 1];
}

void Board3483::Display::write_word(unsigned off, uint16_t value) {
    off &= 0xFE;
    regs_[off >> 1] = value;
    if (off == 0x20) board_.set_window((value & 0x8000) != 0);   // 58021 bit 7
}

unsigned Board3483::Display::rows() const {
    const unsigned n = (regs_[0x06 >> 1] & 0xFF) + 1;   // +06 low byte = rows - 1 [hypothesis]
    return n >= 2 && n <= 62 ? n : 24;
}

unsigned Board3483::Display::columns() const { return (regs_[0] & 0xFF) + 1 >= 132 ? 132 : 80; }

uint32_t Board3483::Display::row_address(unsigned row) const {
    // 5801A bit 6 set: linear, rows follow each other from the start address
    // +1C [observed: POST's own screens set it with +1C=1000]. Clear: the
    // row list in block RAM [observed: svc_244 copies it there], entry r at
    // 50108+4r and 50508+4r; the row starts at cell
    // ((A[0] & 7F) << 8 | B[0]), stored with a bias of 800h cells, so the
    // byte address is twice that [hypothesis, fits the display test: cell
    // 1F5 is cleared regen at 13EA].
    if (regs_[0x1A >> 1] & 0x0040) return regs_[0x1C >> 1] + 2u * row * columns();
    const uint32_t a = 0x50108 + 4 * row, b = 0x50508 + 4 * row;
    const uint32_t cell = uint32_t(board_.bus_.peek8(a) & 0x7F) << 8 | board_.bus_.peek8(b);
    return 2 * cell;
}

bool Board3483::Display::screen_has_ink() const {
    for (unsigned r = 0; r < rows(); ++r) {
        const uint32_t start = row_address(r);
        for (unsigned c = 0; c < columns(); ++c) {
            const uint8_t ch = board_.bus_.peek8(start + 2 * c);
            const uint8_t attr = board_.bus_.peek8(start + 2 * c + 1);
            if (ch != 0x00 && ch != 0x10 && !(attr & 0x20)) {
                LOG_DEBUG("display", "sense: row %u at %05x, cell %u holds %02x", r, start, c, ch);
                return true;
            }
        }
    }
    return false;
}

uint16_t Board3483::Display::mmio_read16(uint32_t off) { return read_word(off); }
void Board3483::Display::mmio_write16(uint32_t off, uint16_t value) { write_word(off, value); }

uint8_t Board3483::Display::mmio_read8(uint32_t off) {
    const uint16_t w = read_word(off);
    return uint8_t(off & 1 ? w >> 8 : w);
}

void Board3483::Display::mmio_write8(uint32_t off, uint8_t value) {
    uint16_t w = regs_[(off & 0xFE) >> 1];
    w = off & 1 ? uint16_t((w & 0x00FF) | (value << 8)) : uint16_t((w & 0xFF00) | value);
    write_word(off, w);
}

uint8_t Board3483::Display::peek8(uint32_t off) { return mmio_read8(off); }

std::string Board3483::Display::describe() const {
    std::string s = "display controller 58000:\n";
    for (unsigned i = 0; i < 0x30; i += 2) s += format("  +%02x %04x%s", i, regs_[i >> 1], (i & 0xE) == 0xE ? "\n" : "");
    s += format("frame %llu, %s\n", (unsigned long long)board_.frame_count_, board_.in_vsync() ? "vsync" : "active");
    return s;
}

void Board3483::Display::render(Framebuffer& fb) {
    // First cut [hypothesis throughout]: cell height from +0A low byte,
    // cells of two bytes (character in 3270 buffer code, attribute) per
    // row_address(), glyphs as 16-bit rows, 32 per character, in the 40000
    // window; 9 dots per cell. Attributes are ignored for now.
    const unsigned cols = columns(), nrows = rows();
    unsigned cell_h = (regs_[0x0A >> 1] & 0xFF) + 1;
    if (cell_h < 8 || cell_h > 32) cell_h = 20;
    const unsigned cell_w = 9;
    fb.width = int(cols * cell_w);
    fb.height = int(nrows * cell_h);
    fb.display_aspect = 4.0 / 3.0;
    fb.pixels.assign(size_t(fb.width) * size_t(fb.height), 0xFF000000u);
    const bool video_on = board_.coax_ && (board_.coax_->reg(0x10) & 1);
    if (!video_on) return;
    for (unsigned r = 0; r < nrows; ++r) {
        const uint32_t start = row_address(r);
        for (unsigned c = 0; c < cols; ++c) {
            const uint8_t ch = board_.bus_.peek8(start + 2 * c);
            if (board_.bus_.peek8(start + 2 * c + 1) & 0x20) continue;   // field attribute: blank [hypothesis]
            for (unsigned y = 0; y < cell_h; ++y) {
                // Glyph row [observed: the firmware decompresses its
                // character sets to 48000 (base set) and 4A000 (APL/GE set),
                // 32 bytes per character, with sparse companion planes at
                // 40000 and 42000; hypothesis: the companion byte holds the
                // dots right of the first 8, as the window's word layout
                // (low byte to 40000, high byte to 48000) suggests].
                const uint32_t at = uint32_t(ch) * 32 + y;
                const uint16_t bits = uint16_t(board_.bus_.peek8(kCgBase + at) << 8 | board_.bus_.peek8(kCgExtension + at));
                uint32_t* row = &fb.pixels[size_t(r * cell_h + y) * size_t(fb.width) + c * cell_w];
                for (unsigned x = 0; x < cell_w; ++x)
                    if (bits & (0x8000u >> x)) row[x] = 0xFF33FF66u;
            }
        }
    }
}

// --- coax adapter and interrupt status (58100) -------------------------------

Board3483::Coax::Coax(Board3483& board) : Device("coax-3483"), board_(board) { board_.coax_ = this; }

void Board3483::Coax::reset() {
    regs_.fill(0);
    addr_ = addr2_ = 0;
    rx_ = 0;
    tx_ = 0;
    lac_hi_ = false;
    mask_ = 0;
    lac_target_ = 0;
    pending_cmd_ = 0;
    board_.status1_ = board_.status2_ = 0;
    board_.update_irq();
}

namespace {

constexpr uint32_t kCoaxBufferBase = 0x1000;   // [hypothesis: the regen buffer]
constexpr uint32_t kCoaxBufferCells = 0x1000;

// Coax words as the firmware writes them to 5810A [observed in the wrap
// test]: low byte = the 8 data/command bits, bit 15 = parity, bits 14-13
// = word type. Commands in the test use 6xxx (POLL 6001, LAC HI 6004) and
// 4xxx (POLL/ACK 4011, LAC LO 4014, WRITE DATA 400C); data words have
// their parity in bit 15.
bool is_command(uint16_t w) { return (w & 0x4000) != 0; }

} // namespace

void Board3483::Coax::transmit(uint16_t word) {
    const uint16_t mode = regs_[0] & 0xF000;
    const bool wrap = mode == 0x7000 || mode == 0xD000;   // internal wrap [observed: POST test 5]
    if (!wrap) {
        LOG_DEBUG("coax", "transmit %04x with no controller attached (mode %04x)", word, regs_[0]);
        return;
    }
    rx_ = word;
    if (is_command(word)) {
        const uint8_t cmd = uint8_t(word);
        pending_cmd_ = word;
        lac_target_ = (cmd & 0xC0) == 0xC0 ? 2 : 1;
        board_.raise1(0x0800);   // command received [observed: tested after the first command]
        const uint8_t base = cmd & 0x3F;
        if (base == 0x11) board_.raise1(0x0100);   // POLL/ACK [observed: test expects 5810C bit 8]
        // The adapter answers with a word [observed in the wrap test]: to
        // POLL, the status from 50308 (low byte = data, bit 0 of the high
        // byte -> bit 13); to anything else 2000 (bit 13 alone, the
        // turnaround reply, hypothesis). Bit 15 is odd parity over the rest:
        // 50308=0102 -> A002, 0001 -> 0001, 0000 -> 2000.
        uint16_t reply = 0x2000;
        if (base == 0x03) {   // READ DATA: the byte at the counter, which advances [observed: test step 52]
            uint16_t& counter = lac_target_ == 2 ? addr2_ : addr_;
            reply = fetch(counter);
            counter = uint16_t(counter + 1);
        } else if (base == 0x01) {
            const uint16_t st = uint16_t(board_.bus_.peek8(0x50308) | board_.bus_.peek8(0x50309) << 8);
            // 0000 = nothing to report: the plain turnaround [observed: 2000].
            if (st) reply = uint16_t(((st >> 8) & 1) << 13 | (st & 0xFF));
        }
        bool odd = false;
        for (uint16_t b = reply; b; b &= uint16_t(b - 1)) odd = !odd;
        regs_[0x0A >> 1] = uint16_t(reply | (odd ? 0 : 0x8000));
        return;
    }
    // A data word after a command.
    const uint8_t cmd = uint8_t(pending_cmd_) & 0x3F;
    const uint8_t data = uint8_t(word);
    uint16_t& counter = lac_target_ == 2 ? addr2_ : addr_;
    if (cmd == 0x04) counter = uint16_t((counter & 0x00FF) | (data << 8));        // LOAD ADDRESS COUNTER HI
    else if (cmd == 0x14) counter = uint16_t((counter & 0xFF00) | data);          // LOAD ADDRESS COUNTER LO
    else if (cmd == 0x16) mask_ = data;                                           // LOAD MASK
    else if (cmd == 0x0C) {
        // WRITE DATA: store and advance. A regen write raises 5810C bit 15
        // [observed: the wrap test checks it after writing to the regen
        // counter, not after writing through the second counter].
        store(counter, data);
        counter = uint16_t(counter + 1);
        if (lac_target_ != 2) board_.raise1(0x8000);
    } else if (cmd == 0x06) {
        // CLEAR: the buffer from the address counter on takes the pattern
        // under the mask [hypothesis for the range; the test reads 00 back].
        for (uint32_t a = counter; a < kCoaxBufferCells; ++a) store(uint16_t(a), data);
    }
    regs_[0x0A >> 1] = 0x2000;
    if (word & 0x1000) board_.raise1(lac_target_ == 2 ? 0x0200 : 0x0400);
}

uint32_t Board3483::Coax::cell_address(uint16_t counter) const {
    // Buffer address -> DRAM: two bytes per cell from 0:1000, the character
    // byte for the regen counter, the next byte for the second (extended
    // attribute) counter [hypothesis].
    return kCoaxBufferBase + 2u * (counter % kCoaxBufferCells) + (lac_target_ == 2 ? 1 : 0);
}

void Board3483::Coax::store(uint16_t counter, uint8_t data) {
    const uint32_t a = cell_address(counter);
    const uint8_t old = board_.bus_.peek8(a);
    board_.bus_.poke8(a, uint8_t((old & mask_) | (data & ~mask_)));
}

uint8_t Board3483::Coax::fetch(uint16_t counter) const { return board_.bus_.peek8(cell_address(counter)); }

void Board3483::Coax::run_engine(uint16_t command) {
    // The ASIC's memory engine [observed: POST display test entries A1/A2,
    // C000:C5D8 and C000:C699]: 58118 = source/pointer, 5811A =
    // destination, 5811C = word count, started by 58110 bit 7, which reads
    // back as busy. Done at once here.
    //  - 08: copy, keeping the character byte and clearing the attribute
    //    byte (38A0 -> 00A0);
    //  - 30: search backward from 58118 for a cell whose attribute byte has
    //    bit 5 clear, leaving 58118 at it [hypothesis for the criterion].
    Bus& bus = board_.bus_;
    const uint16_t op = command & 0x78;
    uint16_t src = regs_[0x18 >> 1], dst = regs_[0x1A >> 1];
    const uint16_t count = regs_[0x1C >> 1];
    if (op == 0x08) {
        for (uint16_t i = 0; i < count; ++i, src += 2, dst += 2) {
            bus.poke8(dst, bus.peek8(src));
            bus.poke8(uint32_t(dst) + 1, 0);
        }
    } else if (op == 0x30) {
        for (uint16_t i = 0; i < count; ++i, src -= 2)
            if (!(bus.peek8(uint32_t(src) + 1) & 0x20)) break;
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
    case 0x04: return addr_;
    case 0x06: return addr2_;
    case 0x08: return rx_;
    case 0x0C: return board_.status1_;
    case 0x10: return uint16_t(regs_[off >> 1] & ~0x0080);   // bit 7 busy: never [observed: polled without timeout]
    case 0x20: return board_.status2_;
    default: return regs_[off >> 1];
    }
}

void Board3483::Coax::write_word(unsigned off, uint16_t value) {
    off &= 0xFE;
    switch (off) {
    case 0x00:
        regs_[0] = value;
        if (value & 0x0100) transmit(tx_);   // bit 8: send the word in +0A [observed: wrap test]
        return;
    case 0x0A:
        tx_ = value;   // reads of +0A give the adapter's status word instead
        return;
    case 0x0C:
        board_.status1_ = uint16_t(board_.status1_ & ~value);
        board_.update_irq();
        return;
    case 0x20:
        board_.status2_ = uint16_t(board_.status2_ & ~value);
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
    default:
        regs_[off >> 1] = value;
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
    return format("coax 58100: control %04x tx %04x rx %04x address %04x/%04x status %04x/%04x\n", regs_[0], tx_,
                  rx_, addr_, addr2_, board_.status1_, board_.status2_);
}

// --- system control (58400) --------------------------------------------------

Board3483::System::System(Board3483& board) : Device("system-3483"), board_(board) { board_.system_ = this; }

void Board3483::System::reset() {
    regs_.fill(0);
    channels_ = {};
    tx_left_ = rx_left_ = hold_left_ = 0;
    rx_active_ = false;
    us_acc_ = 0;
    if (kbd_) kbd_->power_on();
}

void Board3483::System::attach_keyboard(Set3Keyboard* kbd, uint64_t clock_hz) {
    kbd_ = kbd;
    clock_hz_ = clock_hz ? clock_hz : 1;
    byte_cycles_ = clock_hz_ / 1000;   // about 1 ms per 11-bit frame [hypothesis, keyboard clock ~11 kHz]
}

// Sending to the keyboard [observed: svc_113, D000:3116]: the firmware
// writes byte | odd parity (bit 8) | stop (bit 9) with bit 10 low, which
// pulls the data line (request to send), clears the control register and
// polls control bit 2 until the frame is out; data then reads 0 when the
// keyboard acknowledged it.
void Board3483::System::data_write(unsigned channel, uint16_t value) {
    Channel& ch = channels_[channel & 1];
    ch.data = ch.drive = value;
    if ((channel & 1) == 0 && kbd_ && !(value & 0x0400) && !(ch.control & 0x0080)) {
        tx_byte_ = uint8_t(value);
        tx_left_ = byte_cycles_ ? byte_cycles_ : 1;
    }
}

void Board3483::System::tick(uint64_t cycles) {
    if (!kbd_) return;
    us_acc_ += cycles * 1000000;
    const uint64_t us = us_acc_ / clock_hz_;
    us_acc_ %= clock_hz_;
    if (us) kbd_->advance_us(us);
    Channel& a = channels_[0];
    if (tx_left_) {
        if (cycles < tx_left_) {
            tx_left_ -= cycles;
            return;
        }
        tx_left_ = 0;
        LOG_DEBUG("kbd", "host -> keyboard %02X", tx_byte_);
        kbd_->host_write(tx_byte_);
        a.data = 0;              // acknowledged
        a.control |= 0x0004;     // frame out [observed: polled bit]
        // The keyboard answers a few milliseconds later; the firmware sets
        // up its receive wait (clearing the status) in between [observed:
        // an immediate answer is lost].
        hold_left_ = 3 * byte_cycles_;
        return;
    }
    if (hold_left_) {
        hold_left_ = cycles < hold_left_ ? hold_left_ - cycles : 0;
        return;
    }
    // The keyboard sends while the channel is armed and the CPU is not
    // driving the lines.
    if (!a.armed || (a.control & 0x0080) || !kbd_->has_output()) {
        rx_active_ = false;
        return;
    }
    if (!rx_active_) {
        rx_active_ = true;
        rx_left_ = byte_cycles_;
    }
    if (cycles < rx_left_) {
        rx_left_ -= cycles;
        return;
    }
    rx_active_ = false;
    const uint8_t b = kbd_->read_output();
    LOG_DEBUG("kbd", "keyboard -> host %02X", b);
    receive(0, b);
}

uint16_t Board3483::System::sense() const {
    // Bits 1-0 = 11: normal power-on (no test fixture) [observed decoder].
    // Bit 2: vertical sync, low during the pulse [hypothesis, strong]. Bits 8-11: monitor sense
    // lines, line 1 in bit 11, each per its two-bit code [observed decoder].
    uint16_t v = 0x0003;
    if (!board_.in_vsync()) v |= 0x0004;   // 1 during the frame, 0 in the sync pulse [observed: POST windows]
    const uint16_t drive = board_.display_ ? board_.display_->reg(0x26) : 0;
    for (unsigned line = 0; line < 4; ++line) {
        const unsigned code = (board_.monitor_sense_ >> (6 - 2 * line)) & 3;
        bool level = false;
        switch (code) {
        case 0: level = false; break;
        case 1: level = true; break;
        case 2: level = (drive & 0x20) != 0; break;
        case 3: level = (drive & 0x01) != 0; break;
        }
        if (level) v |= uint16_t(0x0800 >> line);
    }
    return v;
}

uint16_t Board3483::System::read_word(unsigned off) const {
    off &= 0xFE;
    switch (off) {
    case 0x08: return channels_[0].data;
    case 0x12: return channels_[1].data;
    case 0x06:
    case 0x10: {
        // Control read: bit 6 is the data line, driven from data register
        // bit 10; the clock line (bit 5) is not driven and reads low
        // [observed: POST tests 14/15, C000:B537-B59C].
        const Channel& ch = channels_[off == 0x06 ? 0 : 1];
        return uint16_t((ch.control & ~0x0060) | (ch.drive & 0x0400 ? 0x0040 : 0));
    }
    case 0x0E: return sense();
    default: return regs_[off >> 1];
    }
}

void Board3483::System::write_word(unsigned off, uint16_t value) {
    off &= 0xFE;
    regs_[off >> 1] = value;
    switch (off) {
    case 0x06:
    case 0x10: control_write(off == 0x06 ? 0 : 1, value); break;
    case 0x08: data_write(0, value); break;
    case 0x12: data_write(1, value); break;
    default: break;
    }
}

// Serial channel control (58406 for A, 58410 for B) [observed: POST tests
// 12/13, C000:B3AB-B46F]: bit 2 arms the receiver; with bit 7 set the CPU
// drives the lines itself, bit 6 = data, bit 5 = clock, and the receiver
// takes the data line on each falling clock edge, least significant bit
// first. After 11 bits (start, 8 data, parity, stop) the frame shifted
// right by one (data in bits 0-7, parity bit 8, stop bit 9) is latched in
// the data register and the channel interrupts [observed: 074A -> 03A5].
void Board3483::System::control_write(unsigned channel, uint16_t value) {
    Channel& ch = channels_[channel & 1];
    const uint16_t old = ch.control;
    ch.control = value;
    // Arming starts a new frame, and so does the CPU taking the lines; the
    // write that does so is not a clock edge [observed: POST re-arms with
    // the clock line dropping between its two test frames].
    const bool arming = (value & 0x0004) && !ch.armed;
    if (value & 0x0004) ch.armed = true;
    if (arming) {
        // Re-arming drops the channel's previous "received" status
        // [hypothesis: the firmware enables the interrupt right after
        // re-arming without clearing it, and a stale one would read FFFF].
        if ((channel & 1) == 0) board_.status1_ = uint16_t(board_.status1_ & ~0x0010);
        else board_.status2_ = uint16_t(board_.status2_ & ~0x8000);
        board_.update_irq();
    }
    if (!(value & 0x0080)) return;
    if (arming || !(old & 0x0080)) {
        ch.shift = 0;
        ch.bits = 0;
        return;
    }
    const bool falling = (old & 0x0020) && !(value & 0x0020);
    if (!falling) return;
    if (value & 0x0040) ch.shift |= uint16_t(1u << ch.bits);
    if (++ch.bits == 11) {
        frame_received(channel, ch.shift);
        ch.shift = 0;
        ch.bits = 0;
    }
}

void Board3483::System::frame_received(unsigned channel, uint16_t frame) {
    Channel& ch = channels_[channel & 1];
    ch.data = uint16_t(frame >> 1);
    if (!ch.armed) return;
    ch.armed = false;
    if ((channel & 1) == 0) board_.raise1(0x0010);
    else board_.raise2(0x8000);
}

bool Board3483::System::receive(unsigned channel, uint8_t byte) {
    Channel& ch = channels_[channel & 1];
    if (!ch.armed) return false;
    bool odd = false;
    for (uint8_t b = byte; b; b &= uint8_t(b - 1)) odd = !odd;
    // start 0, data, odd parity over data + parity, stop 1
    const uint16_t frame = uint16_t((byte << 1) | (odd ? 0 : 0x200) | 0x400);
    frame_received(channel, frame);
    return true;
}

uint16_t Board3483::System::mmio_read16(uint32_t off) { return read_word(off); }
void Board3483::System::mmio_write16(uint32_t off, uint16_t value) { write_word(off, value); }

uint8_t Board3483::System::mmio_read8(uint32_t off) {
    const uint16_t w = read_word(off);
    return uint8_t(off & 1 ? w >> 8 : w);
}

void Board3483::System::mmio_write8(uint32_t off, uint8_t value) {
    const uint16_t cur = regs_[(off & 0xFE) >> 1];
    write_word(off, off & 1 ? uint16_t((cur & 0x00FF) | (value << 8)) : uint16_t((cur & 0xFF00) | value));
}

uint8_t Board3483::System::peek8(uint32_t off) { return mmio_read8(off); }

std::string Board3483::System::describe() const {
    return format("system 58400: indicator %04x progress %04x sense %04x channel A %s %04x, B %s %04x\n", regs_[1],
                  regs_[5], sense(), channels_[0].armed ? "armed" : "idle", channels_[0].data,
                  channels_[1].armed ? "armed" : "idle", channels_[1].data);
}

} // namespace em34xx
