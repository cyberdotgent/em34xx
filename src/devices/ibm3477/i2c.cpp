// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices/ibm3477/i2c.hpp"

#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace em34xx {

I2cEeprom::I2cEeprom(std::string name, size_t size, size_t page_size)
    : Device(std::move(name)), data_(size, 0xFF), page_size_(page_size) {}

I2cEeprom::~I2cEeprom() {
    std::string error;
    if (!flush(error)) LOG_ERROR("eeprom", "%s", error.c_str());
}

void I2cEeprom::attach(I2cBus& bus, uint8_t base_addr8) {
    base_addr7_ = uint8_t(base_addr8 >> 1);
    const size_t blocks = (data_.size() + 255) / 256;
    for (size_t b = 0; b < blocks; ++b) bus.attach(uint8_t(base_addr7_ + b), this);
}

bool I2cEeprom::open(const std::string& path, std::string& error) {
    path_ = path;
    dirty_ = false;
    std::fill(data_.begin(), data_.end(), 0xFF);
    if (path.empty()) {
        LOG_INFO("eeprom", "%s is ephemeral: starts blank, contents are discarded at exit "
                 "(use --eeprom FILE to keep them)", name().c_str());
        return true;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        LOG_INFO("eeprom", "%s: image %s does not exist yet; starting blank, it is created on the first write",
                 name().c_str(), path.c_str());
        return true;
    }
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (image.size() != data_.size()) {
        error = format("EEPROM image %s has %zu bytes, expected %zu", path.c_str(), image.size(), data_.size());
        path_.clear();
        return false;
    }
    data_ = std::move(image);
    LOG_INFO("eeprom", "%s: loaded %s", name().c_str(), path.c_str());
    return true;
}

static bool write_file(const std::string& path, const std::vector<uint8_t>& data, std::string& error) {
    // Write a temporary file and rename it over the image, so a crash never
    // leaves a truncated image behind.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) {
            error = "cannot write " + tmp;
            return false;
        }
    }
    std::remove(path.c_str());   // rename() does not replace on Windows
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "cannot rename " + tmp + " to " + path;
        return false;
    }
    return true;
}

bool I2cEeprom::flush(std::string& error) {
    if (!dirty_ || path_.empty()) return true;
    if (!write_file(path_, data_, error)) return false;
    dirty_ = false;
    return true;
}

bool I2cEeprom::nv_save(const std::string& path, std::string& error) {
    if (path.empty()) {
        if (path_.empty()) {
            error = "the EEPROM is ephemeral; give a file name";
            return false;
        }
        if (!write_file(path_, data_, error)) return false;
        dirty_ = false;
        return true;
    }
    return write_file(path, data_, error);
}

std::string I2cEeprom::nv_status() const {
    return format("%s, %zu bytes, %s, %s", name().c_str(), data_.size(),
                  path_.empty() ? "ephemeral" : path_.c_str(), dirty_ ? "unsaved changes" : "clean");
}

std::string I2cEeprom::describe() const {
    std::string t = "  " + nv_status() + "\n";
    t += format("  pointer %03zX\n", pointer_);
    return t;
}

bool I2cEeprom::i2c_select(uint8_t addr7, bool read) {
    block_ = size_t(addr7 - base_addr7_);
    expect_address_ = !read;
    wrote_ = false;
    return true;
}

bool I2cEeprom::i2c_write(uint8_t b) {
    if (expect_address_) {
        pointer_ = (block_ * 256 + b) % data_.size();
        expect_address_ = false;
        return true;
    }
    // Page write: the low address bits wrap within the page.
    if (data_[pointer_] != b) dirty_ = true;
    data_[pointer_] = b;
    wrote_ = true;
    const size_t page = pointer_ - pointer_ % page_size_;
    pointer_ = page + (pointer_ + 1) % page_size_;
    return true;
}

uint8_t I2cEeprom::i2c_read() {
    const uint8_t v = data_[pointer_];
    pointer_ = (pointer_ + 1) % data_.size();
    return v;
}

void I2cEeprom::i2c_stop() {
    if (!wrote_) return;
    wrote_ = false;
    // Commit to the image at once so that a crash loses nothing.
    std::string error;
    if (!flush(error)) LOG_ERROR("eeprom", "%s", error.c_str());
}

bool I2cLogDevice::i2c_select(uint8_t addr7, bool read) {
    addr8_ = uint8_t((addr7 << 1) | (read ? 1 : 0));
    cur_.clear();
    return true;
}

bool I2cLogDevice::i2c_write(uint8_t b) {
    cur_.push_back(b);
    return true;
}

void I2cLogDevice::i2c_stop() {
    if (cur_.empty()) return;
    std::string bytes;
    for (uint8_t b : cur_) bytes += format(" %02X", b);
    LOG_DEBUG("i2c", "%s (%02X) <-%s", name_.c_str(), addr8_, bytes.c_str());
    last_ = cur_;
    cur_.clear();
}

} // namespace em34xx
