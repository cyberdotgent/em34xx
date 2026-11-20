// Battery-backed static RAM: a range of the machine's RAM whose contents
// survive power-off. The image file is loaded when the machine is built
// (power-on) and written back when it is destroyed (power-off) or on
// request. Without a file the RAM is volatile, as if the battery were
// flat: it starts cleared and nothing is kept.
#pragma once

#include "bus/bus.hpp"
#include "bus/device.hpp"
#include "machine/peripherals.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tn34xx {

class BatteryBackedRam : public Device, public NonVolatileStore {
public:
    // The backed range [base, base + size) must lie in one RAM region.
    BatteryBackedRam(std::string name, Bus& bus, uint32_t base, uint32_t size);
    ~BatteryBackedRam() override;

    // Select the image file. An existing file must hold exactly size()
    // bytes and is copied into RAM; a missing file is created at power-off.
    // An empty path keeps the RAM volatile.
    bool open(const std::string& path, std::string& error);
    bool volatile_ram() const { return path_.empty(); }
    uint32_t size() const { return size_; }
    // Write the backed range to the image file (no-op when volatile).
    bool flush(std::string& error);

    // NonVolatileStore
    std::string nv_status() const override;
    const std::vector<uint8_t>& nv_contents() const override;
    bool nv_save(const std::string& path, std::string& error) override;

    std::string describe() const override { return "  " + nv_status() + "\n"; }

private:
    // The backed bytes, or null if the RAM is no longer mapped.
    const uint8_t* data() const;

    Bus& bus_;
    uint32_t base_, size_;
    std::string path_;
    mutable std::vector<uint8_t> snapshot_;
};

} // namespace tn34xx
