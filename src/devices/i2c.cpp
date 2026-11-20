#include "devices/i2c.hpp"

#include "util/log.hpp"
#include "util/strings.hpp"

namespace tn34xx {

void I2cBus::attach(uint8_t addr7, I2cDevice* dev) { slots_.push_back(Slot{uint8_t(addr7 & 0x7F), dev}); }

bool I2cBus::sda_in() const { return sda_master_ && sda_slave_; }

void I2cBus::reset() {
    scl_ = true;
    sda_master_ = true;
    sda_slave_ = true;
    state_ = State::Idle;
    active_ = nullptr;
    bit_ = 0;
    shift_ = 0;
}

void I2cBus::set_lines(bool scl, bool sda) {
    // When both lines change in one register write, a falling SCL is taken
    // first (data changes while the clock is low) and a rising SCL last
    // (data is set up before the clock rises). Neither order can then
    // produce a spurious START or STOP.
    auto apply_sda = [&] {
        const bool before = sda_in();
        sda_master_ = sda;
        const bool after = sda_in();
        if (scl_ && before != after) {
            if (!after) on_start();
            else on_stop();
        }
    };
    if (scl != scl_ && !scl) {
        scl_ = false;
        on_fall();
        apply_sda();
    } else {
        apply_sda();
        if (scl != scl_) {
            scl_ = scl;
            if (scl) on_rise();
            else on_fall();
        }
    }
}

void I2cBus::on_start() {
    // A repeated START keeps the device's internal address pointer; it is
    // a fresh transaction for the bus.
    state_ = State::Address;
    active_ = nullptr;
    bit_ = 0;
    shift_ = 0;
    sda_slave_ = true;
    ++transactions_;
}

void I2cBus::on_stop() {
    if (active_) active_->i2c_stop();
    active_ = nullptr;
    state_ = State::Idle;
    sda_slave_ = true;
    bit_ = 0;
}

void I2cBus::on_rise() {
    if (state_ == State::Idle || state_ == State::Ignore) return;
    if (bit_ < 8) {
        if (state_ == State::Address || state_ == State::Write) shift_ = uint8_t((shift_ << 1) | (sda_in() ? 1 : 0));
        ++bit_;
        if (bit_ == 8) {
            if (state_ == State::Address) {
                const uint8_t addr7 = uint8_t(shift_ >> 1);
                const bool read = shift_ & 1;
                ack_pending_ = false;
                for (const Slot& s : slots_) {
                    if (s.addr7 == addr7 && s.dev->i2c_select(addr7, read)) {
                        active_ = s.dev;
                        ack_pending_ = true;
                        break;
                    }
                }
                if (!active_) LOG_DEBUG("i2c", "no device acknowledges address %02X", unsigned(shift_ & 0xFE));
                read_mode_ = read;
            } else if (state_ == State::Write) {
                ack_pending_ = active_ && active_->i2c_write(shift_);
            }
        }
        return;
    }
    // Acknowledge slot (ninth clock).
    if (state_ == State::Read) master_nack_ = sda_in();
    bit_ = 9;
}

void I2cBus::on_fall() {
    if (state_ == State::Idle || state_ == State::Ignore) return;
    if (bit_ == 8) {
        // Drive the acknowledge for a byte the slave received; release SDA
        // for the master's acknowledge of a byte it read.
        sda_slave_ = (state_ == State::Read) ? true : !ack_pending_;
        return;
    }
    if (bit_ == 9) {
        sda_slave_ = true;
        bit_ = 0;
        shift_ = 0;
        if (state_ == State::Address) {
            if (!ack_pending_) {
                state_ = State::Ignore;
                return;
            }
            state_ = read_mode_ ? State::Read : State::Write;
        } else if (state_ == State::Read && master_nack_) {
            state_ = State::Ignore;   // master is done; wait for STOP
            return;
        } else if (state_ == State::Write) {
            return;
        }
        if (state_ == State::Read) {
            shift_ = active_->i2c_read();
            sda_slave_ = (shift_ & 0x80) != 0;
        }
        return;
    }
    if (state_ == State::Read && bit_ >= 1 && bit_ < 8) sda_slave_ = ((shift_ >> (7 - bit_)) & 1) != 0;
}

std::string I2cBus::describe() const {
    static const char* names[] = {"idle", "address", "write", "read", "ignore"};
    std::string t = format("  i2c: SCL %d SDA %d, state %s, %llu transactions, devices:", scl_ ? 1 : 0,
                           sda_in() ? 1 : 0, names[int(state_)], (unsigned long long)transactions_);
    for (const Slot& s : slots_) t += format(" %02X", unsigned(s.addr7 << 1));
    return t + "\n";
}

} // namespace tn34xx
