// Display subtype of a terminal. The same firmware drives monochrome and
// colour units, so the subtype is a configuration choice rather than a
// separate model. Monochrome subtypes are rendered by mapping the display
// device's output to the luminance ramp of the relevant phosphor.
#pragma once

#include "machine/peripherals.hpp"

#include <cstdint>
#include <string>

namespace tn34xx {

enum class DisplaySubtype {
    Green,       // monochrome, green phosphor
    Amber,       // monochrome, amber phosphor
    PaperWhite,  // monochrome, paper-white phosphor
    Color,
};

const char* display_subtype_name(DisplaySubtype s);
// Accepts green, amber, white/paperwhite/paper-white, color/colour.
bool parse_display_subtype(const std::string& text, DisplaySubtype& out);
bool is_monochrome(DisplaySubtype s);

// Full-intensity phosphor colour (0xFFRRGGBB) for a monochrome subtype.
uint32_t phosphor_color(DisplaySubtype s);

// Convert a frame rendered by the display device into what the selected
// tube shows. Colour frames pass through unchanged; for monochrome the
// luminance of each pixel scales the phosphor colour.
void apply_display_subtype(DisplaySubtype s, Framebuffer& fb);

} // namespace tn34xx
