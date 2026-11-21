// SPDX-License-Identifier: GPL-3.0-or-later
#include "present/display_profile.hpp"

#include "machine/machine.hpp"

namespace tn34xx {

uint32_t phosphor_color(DisplaySubtype s) {
    switch (s) {
    case DisplaySubtype::Green: return 0xFF66FFB0u;       // mint green, measured on a real 3486 tube
    case DisplaySubtype::Amber: return 0xFFFFB000u;       // P134-like amber
    case DisplaySubtype::PaperWhite: return 0xFFE8E8DCu;  // P4-like, slightly warm
    case DisplaySubtype::Color: return 0xFFFFFFFFu;
    }
    return 0xFFFFFFFFu;
}

void apply_display_subtype(DisplaySubtype s, Framebuffer& fb) {
    if (!is_monochrome(s)) return;
    const uint32_t p = phosphor_color(s);
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
    apply_display_subtype(m.options().display, fb);
}

} // namespace tn34xx
