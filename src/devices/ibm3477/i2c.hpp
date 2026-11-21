// SPDX-License-Identifier: GPL-3.0-or-later
// Devices on the 3477's two-wire bus (bit-banged by the firmware through the
// display controller's register 2012F; see docs/bringup-3477.md).
//
//  - A 24C0x-style serial EEPROM at 8-bit address A0 (write) / A1 (read)
//    holding the setup configuration [observed: the firmware reads and
//    writes it with standard random-read and byte-write transactions].
//  - A write-only device at 8C, probably the monitor's deflection/geometry
//    controller [hypothesis]. It acknowledges and logs what it receives.
#pragma once

#include "bus/device.hpp"
#include "devices/i2c.hpp"
#include "machine/peripherals.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace em34xx {

// 24C01..24C16-style EEPROM: an 8-bit word address, larger parts take the
// upper address bits from the device address (block select). Writes wrap
// within a page as on the real parts and are committed at STOP.
class I2cEeprom : public Device, public I2cDevice, public NonVolatileStore {
public:
    static constexpr size_t kDefaultSize = 256;   // 24C02

    explicit I2cEeprom(std::string name, size_t size = kDefaultSize, size_t page_size = 8);
    ~I2cEeprom() override;

    // Attach at 8-bit base address (A0 for A0/A1), one 7-bit address per
    // 256-byte block.
    void attach(I2cBus& bus, uint8_t base_addr8 = 0xA0);

    // Select the backing image. An existing file must have exactly size()
    // bytes; a missing file is created on the first write. An empty path
    // makes the EEPROM ephemeral (blank, never saved).
    bool open(const std::string& path, std::string& error);
    bool ephemeral() const { return path_.empty(); }
    const std::string& path() const { return path_; }
    bool dirty() const { return dirty_; }
    size_t size() const { return data_.size(); }
    uint8_t byte(size_t addr) const { return data_[addr % data_.size()]; }
    void set_byte(size_t addr, uint8_t v) { data_[addr % data_.size()] = v; dirty_ = true; }

    // Save pending changes to the backing file (no-op when ephemeral).
    bool flush(std::string& error);
    bool flush_nonvolatile(std::string& error) override { return flush(error); }

    // I2cDevice
    bool i2c_select(uint8_t addr7, bool read) override;
    bool i2c_write(uint8_t byte) override;
    uint8_t i2c_read() override;
    void i2c_stop() override;

    // NonVolatileStore
    std::string nv_status() const override;
    const std::vector<uint8_t>& nv_contents() const override { return data_; }
    bool nv_save(const std::string& path, std::string& error) override;

    std::string describe() const override;

private:
    std::vector<uint8_t> data_;
    size_t page_size_;
    uint8_t base_addr7_ = 0x50;
    std::string path_;
    bool dirty_ = false;

    size_t pointer_ = 0;       // current word address
    bool expect_address_ = false;
    bool wrote_ = false;       // data bytes written in this transaction
    size_t block_ = 0;
};

// An acknowledge-everything slave that logs the bytes written to it.
class I2cLogDevice : public I2cDevice {
public:
    explicit I2cLogDevice(std::string name) : name_(std::move(name)) {}
    bool i2c_select(uint8_t addr7, bool read) override;
    bool i2c_write(uint8_t byte) override;
    uint8_t i2c_read() override { return 0xFF; }
    void i2c_stop() override;
    const std::vector<uint8_t>& last_message() const { return last_; }

private:
    std::string name_;
    uint8_t addr8_ = 0;
    std::vector<uint8_t> cur_, last_;
};

} // namespace em34xx
