// How a display subtype looks: monochrome subtypes are rendered by mapping
// the display device's output to the luminance ramp of the relevant
// phosphor.
#pragma once

#include "machine/display_subtype.hpp"
#include "machine/peripherals.hpp"

#include <cstdint>

namespace tn34xx {

class Machine;

// Full-intensity phosphor colour (0xFFRRGGBB) for a monochrome subtype.
uint32_t phosphor_color(DisplaySubtype s);

// Convert a frame rendered by the display device into what the selected
// tube shows. Colour frames pass through unchanged; for monochrome the
// luminance of each pixel scales the phosphor colour.
void apply_display_subtype(DisplaySubtype s, Framebuffer& fb);

// The machine's raw frame with its display subtype applied: what the front
// ends and the monitor's screenshot show.
void render_presented(Machine& m, Framebuffer& fb);

} // namespace tn34xx
