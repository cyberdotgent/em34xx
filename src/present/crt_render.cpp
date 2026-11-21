// SPDX-License-Identifier: GPL-3.0-or-later
#include "present/crt_render.hpp"

#include <algorithm>
#include <cmath>

namespace em34xx {

void crt_fit(const Framebuffer& fb, int max_w, int max_h, int& out_w, int& out_h) {
    const double aspect = fb.display_aspect > 0.0
                              ? fb.display_aspect
                              : double(fb.presented_width()) / std::max(1, fb.presented_height());
    out_w = max_w;
    out_h = int(max_w / aspect + 0.5);
    if (out_h > max_h) {
        out_h = max_h;
        out_w = int(max_h * aspect + 0.5);
    }
    out_w = std::max(out_w, 1);
    out_h = std::max(out_h, 1);
}

namespace {
constexpr int kTaps = 3;
constexpr size_t kTapCount = size_t(2 * kTaps + 1);

bool same_params(const CrtParams& a, const CrtParams& b) {
    return a.enabled == b.enabled && a.beam_sigma == b.beam_sigma && a.spread == b.spread && a.gain == b.gain;
}
} // namespace

void crt_render(const Framebuffer& fb, int out_w, int out_h, const CrtParams& p, std::vector<uint32_t>& out) {
    CrtRenderer r;
    r.render(fb, out_w, out_h, p, out);
}

void CrtRenderer::setup(const Framebuffer& fb, int out_w, int out_h, const CrtParams& p) {
    fb_w_ = fb.width;
    fb_h_ = fb.height;
    out_w_ = out_w;
    out_h_ = out_h;
    border_x_ = fb.border_x;
    border_y_ = fb.border_y;
    params_ = p;
    ox0_ = ox1_ = oy0_ = oy1_ = 0;
    xi_.clear();
    xw_.clear();
    row_line0_.clear();
    row_weights_.clear();
    spread_.clear();
    if (fb.width <= 0 || fb.height <= 0) return;

    // Raster rectangle inside the tube.
    const double rx0 = fb.border_x * out_w, ry0 = fb.border_y * out_h;
    const double rw = (1.0 - 2.0 * fb.border_x) * out_w, rh = (1.0 - 2.0 * fb.border_y) * out_h;
    if (rw <= 0.0 || rh <= 0.0) return;
    const double sx = fb.width / rw;    // source pixels per output pixel
    const double sy = fb.height / rh;   // source lines per output pixel

    ox0_ = std::max(0, int(std::floor(rx0)));
    ox1_ = std::max(ox0_, std::min(out_w, int(std::ceil(rx0 + rw))));
    oy0_ = std::max(0, int(std::floor(ry0)));
    oy1_ = std::max(oy0_, std::min(out_h, int(std::ceil(ry0 + rh))));

    // When several lines fall on one output pixel the gaps cannot be
    // resolved; widen the beam so the image averages instead of aliasing.
    const double sigma = p.enabled ? std::max(p.beam_sigma, 0.5 * sy) : 0.0;
    const double inv2s2 = sigma > 0.0 ? 1.0 / (2.0 * sigma * sigma) : 0.0;
    // Line centres reach full brightness at the base beam width. When the
    // beam is widened to avoid aliasing, scale so a uniformly lit area keeps
    // the brightness it has at the base width.
    auto line_sum = [](double s2inv) {
        double sum = 0.0;
        for (int k = -4; k <= 4; ++k) sum += std::exp(-double(k * k) * s2inv);
        return sum;
    };
    double norm = p.gain;
    if (p.enabled && sigma > p.beam_sigma) {
        const double base = 1.0 / (2.0 * p.beam_sigma * p.beam_sigma);
        norm = p.gain * line_sum(base) / line_sum(inv2s2);
    }

    // Horizontal sampling. CRT mode: Gaussian beam spread over neighbouring
    // dots, normalised so a uniformly lit run keeps its level. Sharp mode:
    // nearest dot.
    const size_t cols = size_t(ox1_ - ox0_);
    const double hs = std::max(p.spread, 0.5 * sx);
    xi_.assign(cols, 0);
    xw_.assign(cols * kTapCount, 0.0f);
    for (int x = ox0_; x < ox1_; ++x) {
        const size_t k = size_t(x - ox0_);
        const double fx = (x + 0.5 - rx0) * sx - 0.5;   // in source dots
        const int c = int(std::floor(fx + 0.5));
        xi_[k] = c;
        if (!p.enabled) continue;
        double sum = 0.0;
        for (int t = -kTaps; t <= kTaps; ++t) {
            const double d = fx - (c + t);
            const double w = std::exp(-d * d / (2.0 * hs * hs));
            xw_[k * kTapCount + size_t(t + kTaps)] = float(w);
            sum += w;
        }
        for (size_t t = 0; t < kTapCount; ++t) xw_[k * kTapCount + t] /= float(sum);
    }

    // Vertical: the raster lines each output row sees, and their weights.
    row_line0_.assign(size_t(oy1_ - oy0_), 0);
    row_weights_.assign(size_t(oy1_ - oy0_), {});
    for (int y = oy0_; y < oy1_; ++y) {
        const double fy = (y + 0.5 - ry0) * sy - 0.5;   // in source lines
        int l0, l1;
        if (p.enabled) {
            l0 = int(std::floor(fy - 3.0 * sigma));
            l1 = int(std::ceil(fy + 3.0 * sigma));
        } else {
            l0 = l1 = int(std::floor(fy + 0.5));
        }
        l0 = std::max(0, l0);
        l1 = std::min(fb.height - 1, l1);
        const size_t r = size_t(y - oy0_);
        row_line0_[r] = l0;
        for (int line = l0; line <= l1; ++line) {
            const double d = fy - line;
            const float w = p.enabled ? float(std::exp(-d * d * inv2s2) * norm) : 1.0f;
            // Lines too far to matter keep a zero weight (skipped).
            row_weights_[r].push_back(w < 1e-3f ? 0.0f : w);
        }
    }
    spread_.assign(size_t(fb.height) * cols * 3, 0.0f);
}

void CrtRenderer::spread_line(const Framebuffer& fb, int line) {
    const size_t cols = xi_.size();
    float* dst = &spread_[size_t(line) * cols * 3];
    const uint32_t* src = fb.pixels.data() + size_t(line) * size_t(fb.width);
    auto px = [&](int i) -> uint32_t { return (i < 0 || i >= fb.width) ? 0xFF000000u : src[i]; };
    for (size_t k = 0; k < cols; ++k) {
        float r = 0, g = 0, b = 0;
        if (!params_.enabled) {
            const uint32_t a = px(xi_[k]);
            r = float((a >> 16) & 0xFF);
            g = float((a >> 8) & 0xFF);
            b = float(a & 0xFF);
        } else {
            const float* wk = &xw_[k * kTapCount];
            for (int t = -kTaps; t <= kTaps; ++t) {
                const uint32_t a = px(xi_[k] + t);
                if ((a & 0xFFFFFFu) == 0) continue;
                const float wt = wk[t + kTaps];
                r += wt * float((a >> 16) & 0xFF);
                g += wt * float((a >> 8) & 0xFF);
                b += wt * float(a & 0xFF);
            }
        }
        dst[3 * k] = r;
        dst[3 * k + 1] = g;
        dst[3 * k + 2] = b;
    }
}

CrtRenderer::Rows CrtRenderer::render(const Framebuffer& fb, int out_w, int out_h, const CrtParams& p,
                                      std::vector<uint32_t>& out) {
    Rows rows;
    const size_t out_size = size_t(std::max(out_w, 0)) * size_t(std::max(out_h, 0));
    const bool full = !valid_ || fb.width != fb_w_ || fb.height != fb_h_ || out_w != out_w_ || out_h != out_h_ ||
                      fb.border_x != border_x_ || fb.border_y != border_y_ || !same_params(p, params_) ||
                      out.size() != out_size || last_.size() != fb.pixels.size();
    if (full) {
        setup(fb, out_w, out_h, p);
        out.assign(out_size, 0xFF000000u);
        last_.clear();
        valid_ = true;
        rows.row0 = 0;
        rows.row1 = out_h;
    }
    if (fb.width <= 0 || fb.height <= 0 || xi_.empty() || oy1_ <= oy0_) {
        last_ = fb.pixels;
        return rows;
    }

    // Raster lines that changed since the last call.
    std::vector<char> changed(size_t(fb.height), 0);
    bool any = false;
    for (int line = 0; line < fb.height; ++line) {
        const size_t off = size_t(line) * size_t(fb.width);
        if (!full && std::equal(fb.pixels.begin() + std::ptrdiff_t(off),
                                fb.pixels.begin() + std::ptrdiff_t(off + size_t(fb.width)),
                                last_.begin() + std::ptrdiff_t(off)))
            continue;
        changed[size_t(line)] = 1;
        any = true;
        spread_line(fb, line);
    }
    last_ = fb.pixels;
    if (!any) return rows;

    const size_t cols = xi_.size();
    std::vector<float> acc(cols * 3);
    int first = -1, last = -1;
    for (int y = oy0_; y < oy1_; ++y) {
        const size_t r = size_t(y - oy0_);
        const std::vector<float>& weights = row_weights_[r];
        const int l0 = row_line0_[r];
        bool dirty = false;
        for (size_t i = 0; i < weights.size() && !dirty; ++i) dirty = changed[size_t(l0) + i] != 0;
        if (!dirty) continue;
        if (first < 0) first = y;
        last = y;
        std::fill(acc.begin(), acc.end(), 0.0f);
        for (size_t i = 0; i < weights.size(); ++i) {
            const float w = weights[i];
            if (w == 0.0f) continue;
            const float* sp = &spread_[(size_t(l0) + i) * cols * 3];
            for (size_t k = 0; k < cols * 3; ++k) acc[k] += w * sp[k];
        }
        uint32_t* dst = out.data() + size_t(y) * size_t(out_w) + size_t(ox0_);
        for (size_t k = 0; k < cols; ++k) {
            const uint32_t cr = uint32_t(std::min(255.0f, acc[3 * k]));
            const uint32_t cg = uint32_t(std::min(255.0f, acc[3 * k + 1]));
            const uint32_t cb = uint32_t(std::min(255.0f, acc[3 * k + 2]));
            dst[k] = 0xFF000000u | (cr << 16) | (cg << 8) | cb;
        }
    }
    if (!full && first >= 0) {
        rows.row0 = first;
        rows.row1 = last + 1;
    }
    return rows;
}

} // namespace em34xx
