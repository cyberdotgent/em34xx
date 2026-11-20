#include "machine/display_profile.hpp"

#include "util/strings.hpp"

namespace tn34xx {

const char* display_subtype_name(DisplaySubtype s) {
    switch (s) {
    case DisplaySubtype::Green: return "green";
    case DisplaySubtype::Amber: return "amber";
    case DisplaySubtype::PaperWhite: return "paperwhite";
    case DisplaySubtype::Color: return "color";
    }
    return "?";
}

bool parse_display_subtype(const std::string& text, DisplaySubtype& out) {
    const std::string t = to_lower(text);
    if (t == "green" || t == "mono-green") out = DisplaySubtype::Green;
    else if (t == "amber" || t == "mono-amber") out = DisplaySubtype::Amber;
    else if (t == "white" || t == "paperwhite" || t == "paper-white" || t == "paper_white") out = DisplaySubtype::PaperWhite;
    else if (t == "color" || t == "colour") out = DisplaySubtype::Color;
    else return false;
    return true;
}

bool is_monochrome(DisplaySubtype s) { return s != DisplaySubtype::Color; }

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

} // namespace tn34xx
