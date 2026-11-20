#include "present/crt_render.hpp"

#include <algorithm>
#include <cmath>

namespace tn34xx {

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

void crt_render(const Framebuffer& fb, int out_w, int out_h, const CrtParams& p, std::vector<uint32_t>& out) {
    out.assign(size_t(out_w) * size_t(out_h), 0xFF000000u);
    if (fb.width <= 0 || fb.height <= 0) return;

    // Raster rectangle inside the tube.
    const double rx0 = fb.border_x * out_w, ry0 = fb.border_y * out_h;
    const double rw = (1.0 - 2.0 * fb.border_x) * out_w, rh = (1.0 - 2.0 * fb.border_y) * out_h;
    if (rw <= 0.0 || rh <= 0.0) return;
    const double sx = fb.width / rw;    // source pixels per output pixel
    const double sy = fb.height / rh;   // source lines per output pixel

    const int ox0 = std::max(0, int(std::floor(rx0)));
    const int ox1 = std::min(out_w, int(std::ceil(rx0 + rw)));
    const int oy0 = std::max(0, int(std::floor(ry0)));
    const int oy1 = std::min(out_h, int(std::ceil(ry0 + rh)));

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

    std::vector<float> line_r(size_t(ox1 - ox0)), line_g(line_r.size()), line_b(line_r.size());
    // Horizontal sampling. CRT mode: Gaussian beam spread over neighbouring
    // dots, normalised so a uniformly lit run keeps its level. Sharp mode:
    // nearest dot.
    const int kTaps = 3;
    const double hs = std::max(p.spread, 0.5 * sx);
    std::vector<int> xi(line_r.size());
    std::vector<float> xw(line_r.size() * size_t(2 * kTaps + 1));
    for (int x = ox0; x < ox1; ++x) {
        const size_t k = size_t(x - ox0);
        const double fx = (x + 0.5 - rx0) * sx - 0.5;   // in source dots
        if (!p.enabled) {
            xi[k] = int(std::floor(fx + 0.5));
            continue;
        }
        const int c = int(std::floor(fx + 0.5));
        xi[k] = c;
        double sum = 0.0;
        for (int t = -kTaps; t <= kTaps; ++t) {
            const double d = fx - (c + t);
            const double w = std::exp(-d * d / (2.0 * hs * hs));
            xw[k * size_t(2 * kTaps + 1) + size_t(t + kTaps)] = float(w);
            sum += w;
        }
        for (int t = 0; t < 2 * kTaps + 1; ++t) xw[k * size_t(2 * kTaps + 1) + size_t(t)] /= float(sum);
    }
    auto px = [&](int line, int i) -> uint32_t {
        if (i < 0 || i >= fb.width) return 0xFF000000u;
        return fb.pixels[size_t(line) * size_t(fb.width) + size_t(i)];
    };

    for (int y = oy0; y < oy1; ++y) {
        const double fy = (y + 0.5 - ry0) * sy - 0.5;   // in source lines
        std::fill(line_r.begin(), line_r.end(), 0.0f);
        std::fill(line_g.begin(), line_g.end(), 0.0f);
        std::fill(line_b.begin(), line_b.end(), 0.0f);

        int l0, l1;
        if (p.enabled) {
            l0 = int(std::floor(fy - 3.0 * sigma));
            l1 = int(std::ceil(fy + 3.0 * sigma));
        } else {
            l0 = l1 = int(std::floor(fy + 0.5));
        }
        for (int line = std::max(0, l0); line <= std::min(fb.height - 1, l1); ++line) {
            const double d = fy - line;
            const float w = p.enabled ? float(std::exp(-d * d * inv2s2) * norm) : 1.0f;
            if (w < 1e-3f) continue;
            for (size_t k = 0; k < line_r.size(); ++k) {
                float r = 0, g = 0, b = 0;
                if (!p.enabled) {
                    const uint32_t a = px(line, xi[k]);
                    r = float((a >> 16) & 0xFF);
                    g = float((a >> 8) & 0xFF);
                    b = float(a & 0xFF);
                } else {
                    const float* wk = &xw[k * size_t(2 * kTaps + 1)];
                    for (int t = -kTaps; t <= kTaps; ++t) {
                        const uint32_t a = px(line, xi[k] + t);
                        if ((a & 0xFFFFFFu) == 0) continue;
                        const float wt = wk[t + kTaps];
                        r += wt * float((a >> 16) & 0xFF);
                        g += wt * float((a >> 8) & 0xFF);
                        b += wt * float(a & 0xFF);
                    }
                }
                line_r[k] += w * r;
                line_g[k] += w * g;
                line_b[k] += w * b;
            }
        }
        uint32_t* dst = out.data() + size_t(y) * size_t(out_w) + size_t(ox0);
        for (size_t k = 0; k < line_r.size(); ++k) {
            const uint32_t r = uint32_t(std::min(255.0f, line_r[k]));
            const uint32_t g = uint32_t(std::min(255.0f, line_g[k]));
            const uint32_t b = uint32_t(std::min(255.0f, line_b[k]));
            dst[k] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

} // namespace tn34xx
