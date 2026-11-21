// SPDX-License-Identifier: GPL-3.0-or-later
// A two-wire (I²C) bus driven by a bit-banging master.
//
// The bus is open-drain: a line is high only when nobody pulls it low. The
// master (e.g. a gate-array register written by firmware) presents the levels
// it drives with set_lines() and samples SDA with sda_in(). The bus decodes
// START/STOP conditions and bytes on SCL edges and talks to attached slave
// devices at byte level. Slaves never stretch the clock here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace em34xx {

// A slave on the bus, addressed by one or more 7-bit addresses.
class I2cDevice {
public:
    virtual ~I2cDevice() = default;
    // The device was addressed after a START (or repeated START). addr7 is
    // the 7-bit address used; read is the R/W bit. Return false to NACK.
    virtual bool i2c_select(uint8_t addr7, bool read) = 0;
    // A byte written by the master. Return true to ACK.
    virtual bool i2c_write(uint8_t byte) = 0;
    // The next byte the master reads.
    virtual uint8_t i2c_read() = 0;
    // STOP condition (or a START that addresses someone else).
    virtual void i2c_stop() {}
};

class I2cBus {
public:
    // Attach a slave at a 7-bit address (several addresses may map to the
    // same device, e.g. the block-select addresses of a larger EEPROM).
    void attach(uint8_t addr7, I2cDevice* dev);

    // Levels driven by the master: true = released (high), false = pulled low.
    void set_lines(bool scl, bool sda);
    // Actual line levels (wired-AND of master and slave drivers).
    bool sda_in() const;
    bool scl_in() const { return scl_; }

    void reset();
    std::string describe() const;

private:
    enum class State { Idle, Address, Write, Read, Ignore };

    void on_start();
    void on_stop();
    void on_rise();   // SCL rising edge: sample SDA
    void on_fall();   // SCL falling edge: slave updates its SDA driver

    struct Slot {
        uint8_t addr7;
        I2cDevice* dev;
    };
    std::vector<Slot> slots_;

    bool scl_ = true;          // master SCL (no stretching, so also the line)
    bool sda_master_ = true;
    bool sda_slave_ = true;    // slave SDA driver (false = pulling low)

    State state_ = State::Idle;
    I2cDevice* active_ = nullptr;
    unsigned bit_ = 0;         // bits 0-7 data, 8 = acknowledge slot
    uint8_t shift_ = 0;
    bool ack_pending_ = false; // slave ACKs on the next acknowledge slot
    bool master_nack_ = false; // master NACKed a read byte
    bool read_mode_ = false;   // R/W bit of the current address byte
    uint64_t transactions_ = 0;
};

} // namespace em34xx
