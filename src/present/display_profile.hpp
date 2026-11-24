// SPDX-License-Identifier: GPL-3.0-or-later
// How a tube looks: monochrome units are rendered by mapping
// the display device's output to the luminance ramp of the relevant
// phosphor.
#pragma once

#include "machine/peripherals.hpp"

#include <cstdint>

namespace em34xx {

class Machine;

// Convert a frame rendered by the display device into what a monochrome
// tube shows: the luminance of each pixel scales the phosphor colour
// (0xFFRRGGBB).
void apply_phosphor(uint32_t phosphor, Framebuffer& fb);

// The machine's raw frame as its tube shows it (Machine::tube): what the front
// ends and the monitor's screenshot show.
void render_presented(Machine& m, Framebuffer& fb);

} // namespace em34xx
