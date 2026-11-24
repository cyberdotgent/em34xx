// SPDX-License-Identifier: GPL-3.0-or-later
// The 3477's logic board: where it keeps the controls of the shared
// hardware (src/hw) that differ between the terminal families, and its
// configuration straps. docs/bringup-3477.md has the observations.
#pragma once

#include "hw/display_controller.hpp"
#include "hw/twinax_engine.hpp"

#include <cstdint>

namespace em34xx {

// Straps of the monochrome 3477 this ROM can be (display type 4, FE0).
constexpr uint8_t kStraps3477Mono = 0x0C;
// Paper-white as the firmware knows it (display type 5, FW0): its own cell
// heights and monitor-control setting. Not selected by any variant (the
// tube is a phosphor of the monochrome unit); reachable with --straps 0E.
constexpr uint8_t kStraps3477PaperWhite = 0x0E;

// The colour monitor's sense code: the only one the colour unit's firmware
// accepts (display type 6, FC0) [observed]. The monochrome firmware
// rejects it.
constexpr uint8_t kStraps3477Colour = 0x02;

// Station channels of the link engine.
constexpr unsigned kLinkChannels3477 = 2;

// The engine's structures in low RAM [observed].
constexpr TwinaxEngineLayout kLinkLayout3477 = {
    0x0060,   // class_table
    0x0080,   // programs
    0x80,     // program_stride
    0x0180,   // descriptors
    0,        // third_programs: two channels
};

// The 3477 [observed unless marked]: retrace 1/90 of the frame, no sync
// polarity control, selector in 2012F bits 6-5, picture on with 20128 bit 4
// [hypothesis], cursor on with 20128 bit 7 [hypothesis], dots as generated.
constexpr DisplayLayout kDisplayLayout3477 = {
    90,             // retrace_divisor
    {0x28, 0x00},   // sync_invert: none
    5,              // signature_selector_shift
    {0x28, 0x10},   // video_on
    {0x28, 0x80},   // cursor_on
    false,          // half_dot_stretch: its strokes are two dots wide in the generator
    0,              // beam_reference_dots: the spread was tuned per dot on this tube
};

// The colour unit: the same controller, but its firmware keeps 20128 bit 4
// clear and writes the register with bit 6 once a screen is ready (40 in
// the display test, C0 in use) [observed], as the 3486 family's does. Bit 6
// stands in for "picture on" [hypothesis].
constexpr DisplayLayout kDisplayLayout3477Colour = {
    90,             // retrace_divisor
    {0x28, 0x00},   // sync_invert: none
    5,              // signature_selector_shift
    {0x28, 0x40},   // video_on
    {0x28, 0x80},   // cursor_on
    false,          // half_dot_stretch
    0,              // beam_reference_dots
};

} // namespace em34xx
