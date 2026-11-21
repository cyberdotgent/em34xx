// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices/battery_ram.hpp"

#include "util/log.hpp"
#include "util/strings.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace tn34xx {

BatteryBackedRam::BatteryBackedRam(std::string name, Bus& bus, uint32_t base, uint32_t size)
    : Device(std::move(name)), bus_(bus), base_(base), size_(size) {
    if (!data()) throw std::logic_error("battery-backed range outside a RAM region");
}

const uint8_t* BatteryBackedRam::data() const {
    // Looked up each time: the monitor can unmap RAM.
    const Region* r = bus_.region_at(base_);
    if (!r || r->kind != Region::Kind::Ram || uint64_t(base_) + size_ > uint64_t(r->base) + r->storage.size())
        return nullptr;
    return r->storage.data() + (base_ - r->base);
}

BatteryBackedRam::~BatteryBackedRam() {
    std::string error;
    if (!flush(error)) LOG_ERROR("nvram", "%s", error.c_str());
}

bool BatteryBackedRam::open(const std::string& path, std::string& error) {
    path_ = path;
    if (path.empty()) {
        LOG_INFO("nvram", "%s is volatile: RAM starts cleared and is not kept at exit "
                 "(use --nvram FILE to keep it, e.g. the setup configuration)", name().c_str());
        return true;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        LOG_INFO("nvram", "%s: image %s does not exist yet; it is created at exit", name().c_str(),
                 path.c_str());
        return true;
    }
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (image.size() != size_) {
        error = format("RAM image %s has %zu bytes, expected %u", path.c_str(), image.size(), size_);
        path_.clear();
        return false;
    }
    for (uint32_t i = 0; i < size_; ++i) bus_.poke8(base_ + i, image[i]);
    LOG_INFO("nvram", "%s: loaded %s", name().c_str(), path.c_str());
    return true;
}

static bool write_image(const std::string& path, const uint8_t* bytes, size_t n, std::string& error) {
    // Write a temporary file and rename it over the image, so a crash never
    // leaves a truncated image behind.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char*>(bytes), std::streamsize(n))) {
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

bool BatteryBackedRam::flush(std::string& error) {
    if (path_.empty()) return true;
    if (!data()) {
        error = name() + ": RAM no longer mapped; image not saved";
        return false;
    }
    return write_image(path_, data(), size_, error);
}

bool BatteryBackedRam::nv_save(const std::string& path, std::string& error) {
    if (path.empty()) {
        if (path_.empty()) {
            error = "the RAM is volatile; give a file name";
            return false;
        }
        return flush(error);
    }
    if (!data()) {
        error = name() + ": RAM no longer mapped";
        return false;
    }
    return write_image(path, data(), size_, error);
}

const std::vector<uint8_t>& BatteryBackedRam::nv_contents() const {
    if (const uint8_t* d = data()) snapshot_.assign(d, d + size_);
    else snapshot_.clear();
    return snapshot_;
}

std::string BatteryBackedRam::nv_status() const {
    return format("%s, %05X-%05X, %s", name().c_str(), base_, base_ + size_ - 1,
                  path_.empty() ? "volatile (no battery image)" : ("kept in " + path_).c_str());
}

} // namespace tn34xx
