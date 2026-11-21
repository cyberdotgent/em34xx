// SPDX-License-Identifier: GPL-3.0-or-later
// Display subtype of a terminal. The same firmware drives monochrome and
// colour units, so the subtype is a configuration choice rather than a
// separate model. How a subtype looks (phosphor colours) is presentation:
// see src/present/display_profile.hpp.
#pragma once

#include <string>

namespace em34xx {

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

} // namespace em34xx
