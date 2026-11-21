// SPDX-License-Identifier: GPL-3.0-or-later
// A bring-up aid: a device that stands in for unknown hardware. It logs
// every access and either behaves as plain storage ("latch" mode) or returns
// a fixed value. Probes can be mapped from the monitor at run time.
#pragma once

#include "bus/device.hpp"

#include <map>
#include <vector>

namespace em34xx {

class ProbeDevice : public Device {
public:
    ProbeDevice(std::string name, uint32_t size, bool latch, uint8_t fixed_value);

    uint8_t mmio_read8(uint32_t offset) override;
    void mmio_write8(uint32_t offset, uint8_t value) override;
    uint8_t io_read8(uint16_t port) override;
    void io_write8(uint16_t port, uint8_t value) override;
    uint8_t peek8(uint32_t offset) override;
    std::string describe() const override;

private:
    struct Stats { uint64_t reads = 0, writes = 0; uint8_t last_write = 0; };
    uint8_t read(uint32_t key);
    void write(uint32_t key, uint8_t value);

    bool latch_;
    uint8_t fixed_;
    std::vector<uint8_t> data_;
    std::map<uint32_t, Stats> stats_;
};

} // namespace em34xx
