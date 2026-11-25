// SPDX-License-Identifier: GPL-3.0-or-later
// Screen burn-in: the wear of the tube's phosphor. A "fun" feature of the
// presentation only; the emulated machine never sees it.
//
// Phosphor that has been lit a lot gives less light, and looks darker and
// browner than fresh phosphor when it is not lit at all. The wear of each
// spot of the glass is the light it has given, weighted by the speed in
// force at the time, and only ever grows. From it:
//
//   depth      = 1 - exp(-wear / kWearTau)          0 fresh .. 1 worn out
//   efficiency = 1 - kMaxLoss * depth               of a lit spot
//
// The map covers the whole glass (raster and border) at a fixed size, so it
// is the same tube whatever model or screen format draws on it.
#pragma once

#include "machine/peripherals.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace em34xx {

enum class BurnSpeed { Off, Slow, Fast, Hyper };

const char* burn_speed_name(BurnSpeed s);                    // "off", "slow", ...
bool parse_burn_speed(const std::string& text, BurnSpeed& out);
// Wear gained per hour of full brightness. Slow: a loss that can just be
// seen (2%) after about 16 hours of the same lit picture and an obvious one
// (20%) after about 230; Fast: 2 and 29 hours; Hyper: 10 seconds and 2.5
// minutes. A real tube takes thousands of hours.
double burn_speed_rate(BurnSpeed s);

constexpr double kWearTau = 271.0;   // wear at which depth is 1 - 1/e
constexpr double kMaxLoss = 0.35;    // a worn-out spot still gives 65%

class TubeWear {
public:
    static constexpr int kWidth = 1024, kHeight = 768;

    TubeWear();

    // The tube showed `fb` (as presented, before any wear is applied) for
    // `seconds`. Does nothing at BurnSpeed::Off.
    void expose(const Framebuffer& fb, double seconds, BurnSpeed speed);
    // A new tube.
    void reset();

    // Depth (0..1) at a point of the glass, u and v in 0..1, interpolated.
    float depth(double u, double v) const;
    // Changes when the picture drawn from the map may have changed.
    uint64_t version() const { return version_; }
    // Hours of exposure this tube has had, at any speed.
    double hours() const { return hours_; }
    bool fresh() const { return peak_ == 0.0f; }
    // Depth of the most worn spot.
    float peak_depth() const;
    // True when the map has changed since it was read or marked as saved.
    bool unsaved() const { return unsaved_; }
    void mark_saved() { unsaved_ = false; }

    // The tube as a file's contents, and back. A file of another size or
    // version is refused and leaves the tube as it was.
    std::string serialize() const;
    bool deserialize(const std::string& data, std::string& error);

private:
    std::vector<float> wear_;
    double hours_ = 0.0;
    float peak_ = 0.0f;           // largest wear in the map
    double since_version_ = 0.0;  // largest depth change since version_ moved
    uint64_t version_ = 1;
    bool unsaved_ = false;
};

// What the glass shows where the beam gives no light: a dark grey that
// worn phosphor turns darker and browner (0xFFRRGGBB).
uint32_t tube_face_colour(float depth);

} // namespace em34xx
