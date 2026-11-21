// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices/ibm3477/keyboard_if.hpp"

#include "devices/ibm3477/system.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

namespace em34xx {

namespace {
constexpr uint8_t kHold = 0x01;
constexpr uint8_t kClock = 0x02;
constexpr unsigned kWrapBits = 11;

// Odd parity: the parity bit makes the total number of ones odd.
bool odd_parity_bit(uint8_t b) {
    int n = 0;
    for (int i = 0; i < 8; ++i) n += (b >> i) & 1;
    return (n & 1) == 0;
}
} // namespace

Ibm3477KeyboardInterface::Ibm3477KeyboardInterface(uint64_t clock_hz, KeyboardType type, uint8_t id1, uint8_t id2)
    : kbd_(type, id1, id2), clock_hz_(clock_hz ? clock_hz : 1000000),
      byte_cycles_(clock_hz_ * kByteTimeUs / 1000000) {}

void Ibm3477KeyboardInterface::reset() {
    ctl_ = kHold | kClock;
    tx_ = 0;
    tx_loaded_ = false;
    tx_left_ = 0;
    wrap_clocks_ = 0;
    rx_ = 0;
    rx_full_ = false;
    tx_done_ = false;
    rx_parity_ = false;
    rx_left_ = 0;
    rx_active_ = false;
    us_acc_ = 0;
    if (ic_) ic_->lower(Ibm3477Interrupts::Source::Keyboard);
    kbd_.power_on();
}

uint8_t Ibm3477KeyboardInterface::status() const {
    return uint8_t((rx_parity_ ? 0x01 : 0) | (tx_left_ ? 0x02 : 0) | (rx_full_ ? 0x04 : 0));
}

uint8_t Ibm3477KeyboardInterface::read(unsigned reg) const {
    switch (reg) {
    case 0: return rx_;
    case 1: return status();
    default: return 0xFF;
    }
}

void Ibm3477KeyboardInterface::set_rx(uint8_t byte) {
    rx_ = byte;
    rx_parity_ = odd_parity_bit(byte);
    rx_full_ = true;
    ++received_;
    if (ic_) ic_->raise(Ibm3477Interrupts::Source::Keyboard);
}

void Ibm3477KeyboardInterface::write(unsigned reg, uint8_t v) {
    switch (reg) {
    case 0:
        tx_ = v;
        tx_loaded_ = true;
        wrap_clocks_ = 0;
        break;
    case 1: {
        const uint8_t old = ctl_;
        ctl_ = uint8_t(v & 3);
        if ((old & kHold) && (ctl_ & kHold) && !(old & kClock) && (ctl_ & kClock)) {
            // Manual clock while the line is held: shift the loop by one bit.
            if (tx_loaded_ && ++wrap_clocks_ >= kWrapBits) {
                tx_loaded_ = false;
                wrap_clocks_ = 0;
                LOG_DEBUG("kbd", "wrap: %02X looped back", tx_);
                set_rx(tx_);
            }
        }
        if ((old & kHold) && !(ctl_ & kHold) && (ctl_ & kClock) && tx_loaded_ && !tx_left_) {
            // Line released with a byte waiting: the host sends first; a
            // keyboard transmission in progress is abandoned and retried.
            tx_loaded_ = false;
            tx_left_ = byte_cycles_ ? byte_cycles_ : 1;
            rx_active_ = false;
        }
        break;
    }
    case 2:
        if (rx_full_ || tx_done_) {
            rx_full_ = false;
            tx_done_ = false;
            if (ic_) ic_->lower(Ibm3477Interrupts::Source::Keyboard);
        }
        break;
    default:
        break;
    }
}

void Ibm3477KeyboardInterface::tick(uint64_t cycles) {
    // Keyboard time (typematic repeat).
    us_acc_ += cycles * 1000000;
    const uint64_t us = us_acc_ / clock_hz_;
    us_acc_ %= clock_hz_;
    if (us) kbd_.advance_us(us);

    if (tx_left_) {
        if (cycles < tx_left_) {
            tx_left_ -= cycles;
            return;
        }
        tx_left_ = 0;
        ++sent_;
        LOG_DEBUG("kbd", "host -> keyboard %02X", tx_);
        kbd_.host_write(tx_);
        // Transmission complete: the interface interrupts (observed: the
        // firmware waits for an interrupt after each byte it sends and
        // discards it before expecting the keyboard's answer).
        tx_done_ = true;
        if (ic_) ic_->raise(Ibm3477Interrupts::Source::Keyboard);
        return;
    }
    // The keyboard may send while the line is released (02) and the
    // previous byte or transmit completion has been taken.
    if ((ctl_ & 3) != kClock || rx_full_ || tx_done_ || !kbd_.has_output()) {
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
    const uint8_t b = kbd_.read_output();
    LOG_DEBUG("kbd", "keyboard -> host %02X", b);
    set_rx(b);
}

std::string Ibm3477KeyboardInterface::describe() const {
    return format("  keyboard: control %02X status %02X%s, rx %02X, tx %02X%s, %llu sent, %llu received, "
                  "%zu queued, ID %02X %02X\n",
                  ctl_, status(), tx_done_ ? " (sent)" : "", rx_, tx_, tx_loaded_ ? " (loaded)" : "", (unsigned long long)sent_,
                  (unsigned long long)received_, kbd_.output_size(), kbd_.id1(), kbd_.id2());
}

} // namespace em34xx
