// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices/ibm3477/i2c.hpp"

#include "util/file_io.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>
#include <fstream>

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
    (void)error;   // an unusable image is no longer an error
    path_ = path;
    dirty_ = false;
    std::fill(data_.begin(), data_.end(), 0xFF);
    if (path.empty()) {
        LOG_INFO("eeprom", "%s is ephemeral: starts blank, contents are discarded at exit "
                 "(use --eeprom FILE to keep them)", name().c_str());
        return true;
    }
    uint64_t on_disk = 0;
    std::ifstream f(path_from_utf8(path), std::ios::binary);
    if (!f || !file_size(path, on_disk)) {
        LOG_INFO("eeprom", "%s: image %s does not exist yet; starting blank, it is created on the first write",
                 name().c_str(), path.c_str());
        return true;
    }
    // The size is checked before anything is read.
    std::vector<uint8_t> image(on_disk == data_.size() ? data_.size() : 0);
    if (on_disk != data_.size() ||
        !f.read(reinterpret_cast<char*>(image.data()), std::streamsize(image.size()))) {
        // Not this EEPROM's image. Refusing it would stop every start
        // until the file is deleted by hand: keep it under another name
        // and start blank, as with no image.
        f.close();
        std::string why;
        const bool kept = replace_file(path, path + ".bad", why);
        LOG_WARN("eeprom", "%s: image %s has %llu bytes, expected %zu; starting blank, %s", name().c_str(),
                 path.c_str(), static_cast<unsigned long long>(on_disk), data_.size(),
                 kept ? ("the old file is kept as " + path + ".bad").c_str()
                      : ("it is overwritten on the first write (" + why + ")").c_str());
        return true;
    }
    data_ = std::move(image);
    LOG_INFO("eeprom", "%s: loaded %s", name().c_str(), path.c_str());
    return true;
}

static bool write_file(const std::string& path, const std::vector<uint8_t>& data, std::string& error) {
    // Through a temporary file that replaces the image in one step, so a
    // crash or a full disk never costs the previous image.
    return write_file_atomic(path, data.data(), data.size(), error);
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
    // Commit to the image at once so that a crash loses nothing. flush()
    // does nothing when the write changed no byte.
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
