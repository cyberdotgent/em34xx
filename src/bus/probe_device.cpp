#include "bus/probe_device.hpp"

#include "util/log.hpp"
#include "util/strings.hpp"

namespace tn34xx {

ProbeDevice::ProbeDevice(std::string name, uint32_t size, bool latch, uint8_t fixed_value)
    : Device(std::move(name)), latch_(latch), fixed_(fixed_value), data_(size, fixed_value) {}

uint8_t ProbeDevice::read(uint32_t key) {
    Stats& s = stats_[key];
    ++s.reads;
    uint8_t v = latch_ && key < data_.size() ? data_[key] : fixed_;
    LOG_INFO("probe", "%s: read  +%04x -> %02x", name().c_str(), key, v);
    return v;
}

void ProbeDevice::write(uint32_t key, uint8_t value) {
    Stats& s = stats_[key];
    ++s.writes;
    s.last_write = value;
    if (latch_ && key < data_.size()) data_[key] = value;
    LOG_INFO("probe", "%s: write +%04x <- %02x", name().c_str(), key, value);
}

uint8_t ProbeDevice::mmio_read8(uint32_t offset) { return read(offset); }
void ProbeDevice::mmio_write8(uint32_t offset, uint8_t value) { write(offset, value); }
uint8_t ProbeDevice::io_read8(uint16_t port) { return read(port); }
void ProbeDevice::io_write8(uint16_t port, uint8_t value) { write(port, value); }

uint8_t ProbeDevice::peek8(uint32_t offset) {
    return latch_ && offset < data_.size() ? data_[offset] : fixed_;
}

std::string ProbeDevice::describe() const {
    std::string out = format("probe, %s, %zu location(s) touched\n",
                             latch_ ? "latching" : format("fixed value %02x", fixed_).c_str(), stats_.size());
    for (const auto& kv : stats_)
        out += format("  +%04x  reads %-8llu writes %-8llu last write %02x\n", kv.first,
                      (unsigned long long)kv.second.reads, (unsigned long long)kv.second.writes,
                      kv.second.last_write);
    return out;
}

} // namespace tn34xx
