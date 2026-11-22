// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices/battery_ram.hpp"

#include "util/file_io.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <fstream>
#include <stdexcept>

namespace em34xx {

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
    (void)error;   // an unusable image is no longer an error
    path_ = path;
    if (path.empty()) {
        LOG_INFO("nvram", "%s is volatile: RAM starts cleared and is not kept at exit "
                 "(use --nvram FILE to keep it, e.g. the setup configuration)", name().c_str());
        return true;
    }
    uint64_t on_disk = 0;
    std::ifstream f(path_from_utf8(path), std::ios::binary);
    if (!f || !file_size(path, on_disk)) {
        LOG_INFO("nvram", "%s: image %s does not exist yet; it is created at exit", name().c_str(),
                 path.c_str());
        return true;
    }
    // The size is checked before anything is read.
    std::vector<uint8_t> image(on_disk == size_ ? size_ : 0);
    if (on_disk != size_ || !f.read(reinterpret_cast<char*>(image.data()), std::streamsize(size_))) {
        // Not this RAM's image. Refusing it would stop every start until
        // the file is deleted by hand: keep it under another name and
        // start cleared, as with no image.
        f.close();
        std::string why;
        const bool kept = replace_file(path, path + ".bad", why);
        LOG_WARN("nvram", "%s: image %s has %llu bytes, expected %u; starting cleared, %s", name().c_str(),
                 path.c_str(), static_cast<unsigned long long>(on_disk), size_,
                 kept ? ("the old file is kept as " + path + ".bad").c_str()
                      : ("it is overwritten at exit (" + why + ")").c_str());
        return true;
    }
    for (uint32_t i = 0; i < size_; ++i) bus_.poke8(base_ + i, image[i]);
    LOG_INFO("nvram", "%s: loaded %s", name().c_str(), path.c_str());
    return true;
}

static bool write_image(const std::string& path, const uint8_t* bytes, size_t n, std::string& error) {
    // Through a temporary file that replaces the image in one step, so a
    // crash or a full disk never costs the previous image.
    return write_file_atomic(path, bytes, n, error);
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

} // namespace em34xx
