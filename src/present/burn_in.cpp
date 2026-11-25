// SPDX-License-Identifier: GPL-3.0-or-later
#include "present/burn_in.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace em34xx {

namespace {
constexpr char kMagic[8] = {'E', 'M', '3', '4', 'T', 'U', 'B', 'E'};
constexpr uint32_t kFileVersion = 1;
constexpr size_t kHeaderSize = sizeof(kMagic) + 3 * sizeof(uint32_t) + sizeof(double);
constexpr size_t kCells = size_t(TubeWear::kWidth) * size_t(TubeWear::kHeight);
// The picture is redrawn from the map when some spot's depth has moved by
// this much: half a step of an 8-bit level at full loss.
constexpr double kVersionStep = 0.004;
// The unlit glass, 0..255.
constexpr float kFaceLevel = 12.0f;

float depth_of(float wear) { return 1.0f - std::exp(-wear / float(kWearTau)); }
} // namespace

const char* burn_speed_name(BurnSpeed s) {
    switch (s) {
    case BurnSpeed::Off: return "off";
    case BurnSpeed::Slow: return "slow";
    case BurnSpeed::Fast: return "fast";
    case BurnSpeed::Hyper: return "hyper";
    }
    return "off";
}

bool parse_burn_speed(const std::string& text, BurnSpeed& out) {
    for (BurnSpeed s : {BurnSpeed::Off, BurnSpeed::Slow, BurnSpeed::Fast, BurnSpeed::Hyper})
        if (text == burn_speed_name(s)) {
            out = s;
            return true;
        }
    return false;
}

double burn_speed_rate(BurnSpeed s) {
    switch (s) {
    case BurnSpeed::Off: return 0.0;
    case BurnSpeed::Slow: return 1.0;
    case BurnSpeed::Fast: return 8.0;
    case BurnSpeed::Hyper: return 5760.0;
    }
    return 0.0;
}

TubeWear::TubeWear() : wear_(kCells, 0.0f) {}

void TubeWear::reset() {
    std::fill(wear_.begin(), wear_.end(), 0.0f);
    hours_ = 0.0;
    peak_ = 0.0f;
    since_version_ = 0.0;
    ++version_;
    unsaved_ = true;
}

void TubeWear::expose(const Framebuffer& fb, double seconds, BurnSpeed speed) {
    const double rate = burn_speed_rate(speed);
    if (rate <= 0.0 || seconds <= 0.0 || fb.width <= 0 || fb.height <= 0) return;
    const double rw = 1.0 - 2.0 * fb.border_x, rh = 1.0 - 2.0 * fb.border_y;
    if (rw <= 0.0 || rh <= 0.0) return;
    hours_ += seconds / 3600.0;

    // Mean brightness of the raster over each cell of the map it covers
    // (a cell smaller than a raster pixel takes the pixel it lies in). One
    // phosphor layer: the brightest gun stands for the spot.
    struct Span {
        int a, b;   // raster pixels [a, b)
    };
    auto spans = [](double border, double extent, int cells, int pixels, int& first) {
        std::vector<Span> out;
        first = std::max(0, int(std::ceil(border * cells - 0.5)));
        for (int g = first; g < cells; ++g) {
            const double lo = ((g / double(cells)) - border) / extent * pixels;
            const double hi = (((g + 1) / double(cells)) - border) / extent * pixels;
            if (lo >= pixels) break;
            const int a = std::max(0, std::min(pixels - 1, int(std::floor(lo + 0.5))));
            out.push_back({a, std::max(a + 1, std::min(pixels, int(std::floor(hi + 0.5))))});
        }
        return out;
    };
    int gx0 = 0, gy0 = 0;
    const std::vector<Span> xs = spans(fb.border_x, rw, kWidth, fb.width, gx0);
    const std::vector<Span> ys = spans(fb.border_y, rh, kHeight, fb.height, gy0);
    const float gain = float(rate * seconds / 3600.0 / 255.0);
    float step = 0.0f;
    for (size_t j = 0; j < ys.size(); ++j) {
        float* row = &wear_[(size_t(gy0) + j) * kWidth + size_t(gx0)];
        for (size_t i = 0; i < xs.size(); ++i) {
            uint32_t sum = 0;
            for (int y = ys[j].a; y < ys[j].b; ++y) {
                const uint32_t* src = fb.pixels.data() + size_t(y) * size_t(fb.width);
                for (int x = xs[i].a; x < xs[i].b; ++x) {
                    const uint32_t p = src[x];
                    sum += std::max({(p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF});
                }
            }
            if (sum == 0) continue;
            const float before = depth_of(row[i]);
            row[i] += gain * float(sum) / float((ys[j].b - ys[j].a) * (xs[i].b - xs[i].a));
            peak_ = std::max(peak_, row[i]);
            step = std::max(step, depth_of(row[i]) - before);
            unsaved_ = true;
        }
    }
    since_version_ += step;
    if (since_version_ >= kVersionStep) {
        since_version_ = 0.0;
        ++version_;
    }
}

float TubeWear::peak_depth() const { return depth_of(peak_); }

float TubeWear::depth(double u, double v) const {
    if (peak_ == 0.0f) return 0.0f;
    const double fx = std::min(std::max(u * kWidth - 0.5, 0.0), double(kWidth - 1));
    const double fy = std::min(std::max(v * kHeight - 0.5, 0.0), double(kHeight - 1));
    const int x0 = int(fx), y0 = int(fy);
    const int x1 = std::min(x0 + 1, kWidth - 1), y1 = std::min(y0 + 1, kHeight - 1);
    const float ax = float(fx - x0), ay = float(fy - y0);
    const float* r0 = &wear_[size_t(y0) * kWidth];
    const float* r1 = &wear_[size_t(y1) * kWidth];
    const float w = (r0[x0] * (1 - ax) + r0[x1] * ax) * (1 - ay) + (r1[x0] * (1 - ax) + r1[x1] * ax) * ay;
    return depth_of(w);
}

std::string TubeWear::serialize() const {
    std::string out(kHeaderSize + kCells * sizeof(float), '\0');
    char* p = &out[0];
    const uint32_t head[3] = {kFileVersion, uint32_t(kWidth), uint32_t(kHeight)};
    std::memcpy(p, kMagic, sizeof(kMagic));
    std::memcpy(p + sizeof(kMagic), head, sizeof(head));
    std::memcpy(p + sizeof(kMagic) + sizeof(head), &hours_, sizeof(hours_));
    std::memcpy(p + kHeaderSize, wear_.data(), kCells * sizeof(float));
    return out;
}

bool TubeWear::deserialize(const std::string& data, std::string& error) {
    uint32_t head[3];
    if (data.size() < kHeaderSize || std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) {
        error = "not a tube file";
        return false;
    }
    std::memcpy(head, data.data() + sizeof(kMagic), sizeof(head));
    if (head[0] != kFileVersion || head[1] != uint32_t(kWidth) || head[2] != uint32_t(kHeight) ||
        data.size() != kHeaderSize + kCells * sizeof(float)) {
        error = "tube file of another version or size";
        return false;
    }
    std::vector<float> wear(kCells);
    double hours = 0.0;
    std::memcpy(&hours, data.data() + sizeof(kMagic) + sizeof(head), sizeof(hours));
    std::memcpy(wear.data(), data.data() + kHeaderSize, kCells * sizeof(float));
    float peak = 0.0f;
    for (float w : wear) {
        if (!(w >= 0.0f) || !std::isfinite(w)) {
            error = "tube file is damaged";
            return false;
        }
        peak = std::max(peak, w);
    }
    wear_.swap(wear);
    hours_ = std::isfinite(hours) && hours >= 0.0 ? hours : 0.0;
    peak_ = peak;
    since_version_ = 0.0;
    ++version_;
    unsaved_ = false;
    return true;
}

uint32_t tube_face_colour(float depth) {
    const uint32_t r = uint32_t(kFaceLevel * (1.0f - 0.60f * depth) + 0.5f);
    const uint32_t g = uint32_t(kFaceLevel * (1.0f - 0.80f * depth) + 0.5f);
    const uint32_t b = uint32_t(kFaceLevel * (1.0f - 0.95f * depth) + 0.5f);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

} // namespace em34xx
