// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices/ibm3486/aux_port.hpp"

#include "devices/ibm3477/system.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

namespace em34xx {

namespace {
constexpr uint8_t kHold = 0x01;
constexpr uint8_t kClock = 0x02;
constexpr unsigned kWrapBits = 11;   // start, 8 data, parity, stop

// Odd parity: the parity bit makes the total number of ones odd.
bool odd_parity_bit(uint8_t b) {
    int n = 0;
    for (int i = 0; i < 8; ++i) n += (b >> i) & 1;
    return (n & 1) == 0;
}
} // namespace

void Ibm3486AuxPort::reset() {
    ctl_ = kHold | kClock;
    tx_ = 0;
    tx_loaded_ = false;
    wrap_clocks_ = 0;
    rx_ = 0;
    rx_full_ = false;
    rx_parity_ = false;
    tx_left_ = 0;
    tx_done_ = false;
    rx_left_ = 0;
    rx_active_ = false;
    if (ic_) ic_->lower(Ibm3477Interrupts::Source::Aux);
    if (has_mouse_) mouse_.power_on();
}

uint8_t Ibm3486AuxPort::read(unsigned reg) const {
    switch (reg) {
    case 0: return rx_;
    case 1: return uint8_t((rx_parity_ ? 0x01 : 0) | (tx_left_ ? 0x02 : 0) | (rx_full_ ? 0x04 : 0));
    default: return 0xFF;
    }
}

void Ibm3486AuxPort::write(unsigned reg, uint8_t v) {
    switch (reg) {
    case 0:
        tx_ = v;
        tx_loaded_ = true;
        wrap_clocks_ = 0;
        break;
    case 1: {
        const uint8_t old = ctl_;
        ctl_ = uint8_t(v & 3);
        if ((ctl_ & kHold) && tx_left_) {
            // The host takes the line back while a byte is on its way: the
            // device never sees it [hypothesis; observed need: the firmware
            // starts its reset this way while an earlier byte is still
            // going out, and then reads "not busy"].
            LOG_DEBUG("aux", "host -> %02X abandoned (line held)", tx_);
            tx_left_ = 0;
        }
        if ((old & kHold) && !(ctl_ & kHold) && (ctl_ & kClock) && tx_loaded_ && !tx_left_) {
            // Line released with a byte waiting: the host sends first.
            tx_loaded_ = false;
            if (has_mouse_) {
                tx_left_ = byte_cycles_ ? byte_cycles_ : 1;
                rx_active_ = false;
            } else {
                LOG_DEBUG("aux", "host -> %02X, no device", tx_);
            }
        }
        if ((old & kHold) && (ctl_ & kHold) && !(old & kClock) && (ctl_ & kClock) && tx_loaded_ &&
            ++wrap_clocks_ >= kWrapBits) {
            tx_loaded_ = false;
            wrap_clocks_ = 0;
            rx_ = tx_;
            rx_parity_ = odd_parity_bit(rx_);
            rx_full_ = true;
            ++wrapped_;
            LOG_DEBUG("aux", "wrap: %02X looped back", rx_);
            if (ic_) ic_->raise(Ibm3477Interrupts::Source::Aux);
        }
        break;
    }
    case 2:
        rx_full_ = false;
        tx_done_ = false;
        if (ic_) ic_->lower(Ibm3477Interrupts::Source::Aux);
        break;
    default:
        break;
    }
}

void Ibm3486AuxPort::tick(uint64_t cycles) {
    if (!has_mouse_) return;
    if (tx_left_) {
        if (cycles < tx_left_) {
            tx_left_ -= cycles;
            return;
        }
        tx_left_ = 0;
        ++sent_;
        LOG_DEBUG("aux", "host -> mouse %02X", tx_);
        mouse_.host_write(tx_);
        // Transmission complete: the port interrupts, as the keyboard port
        // does (observed: the firmware flags the interrupt it expects after
        // a byte it sent and discards it).
        tx_done_ = true;
        if (ic_) ic_->raise(Ibm3477Interrupts::Source::Aux);
        return;
    }
    // The device may send while the line is released and the previous byte
    // or transmit completion has been taken.
    if ((ctl_ & 3) != kClock || rx_full_ || tx_done_ || !mouse_.has_output()) {
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
    rx_ = mouse_.read_output();
    rx_parity_ = odd_parity_bit(rx_);
    rx_full_ = true;
    ++received_;
    LOG_DEBUG("aux", "mouse -> host %02X", rx_);
    if (ic_) ic_->raise(Ibm3477Interrupts::Source::Aux);
}

std::string Ibm3486AuxPort::describe() const {
    std::string t = format("  control %02X, rx %02X%s, tx %02X%s, %llu byte(s) wrapped, %llu sent, %llu received\n",
                           ctl_, rx_, rx_full_ ? " (received)" : "", tx_, tx_loaded_ ? " (waiting)" : "",
                           (unsigned long long)wrapped_, (unsigned long long)sent_, (unsigned long long)received_);
    t += has_mouse_ ? "  " + mouse_.describe() + "\n" : std::string("  no device\n");
    return t;
}

} // namespace em34xx
