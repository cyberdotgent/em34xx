// SPDX-License-Identifier: GPL-3.0-or-later
#include "present/display_profile.hpp"

#include "machine/machine.hpp"

namespace em34xx {

void apply_phosphor(uint32_t p, Framebuffer& fb) {
    const uint32_t pr = (p >> 16) & 0xFF, pg = (p >> 8) & 0xFF, pb = p & 0xFF;
    for (uint32_t& px : fb.pixels) {
        const uint32_t r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
        // Rec. 601 luma, 0..255.
        const uint32_t y = (r * 299 + g * 587 + b * 114) / 1000;
        px = 0xFF000000u | ((pr * y / 255) << 16) | ((pg * y / 255) << 8) | (pb * y / 255);
    }
}

void render_presented(Machine& m, Framebuffer& fb) {
    m.render(fb);
    if (!m.tube().colour) apply_phosphor(m.tube().phosphor, fb);
}

} // namespace em34xx
