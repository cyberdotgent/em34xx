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
    if (ic_) ic_->lower(Ibm3477Interrupts::Source::Aux);
}

uint8_t Ibm3486AuxPort::read(unsigned reg) const {
    switch (reg) {
    case 0: return rx_;
    case 1: return uint8_t((rx_parity_ ? 0x01 : 0) | (rx_full_ ? 0x40 : 0));
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
        if (ic_) ic_->lower(Ibm3477Interrupts::Source::Aux);
        break;
    default:
        break;
    }
}

std::string Ibm3486AuxPort::describe() const {
    return format("  control %02X, rx %02X%s, tx %02X%s, %llu byte(s) wrapped, no device\n", ctl_, rx_,
                  rx_full_ ? " (received)" : "", tx_, tx_loaded_ ? " (waiting)" : "",
                  (unsigned long long)wrapped_);
}

} // namespace em34xx
