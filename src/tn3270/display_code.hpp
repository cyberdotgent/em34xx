// SPDX-License-Identifier: GPL-3.0-or-later
// The character code of a 3270 CUT terminal's buffer ("display code") and
// its translation from EBCDIC.
//
// A CUT terminal shows what its controller writes into its buffer: one
// byte per position in the terminal's own code, not EBCDIC. The table here
// is the character each code shows on the 3483 [observed: the glyphs of
// the base character set its firmware loads, read one by one], so a
// controller can translate the host's EBCDIC through a code page's
// Unicode. Codes C0-FF are field attributes in the main buffer and status
// symbols on the status line; they are not characters.
#pragma once

#include <cstdint>
#include <string>

namespace em34xx {
namespace tn3270 {

// The character a display code shows (0 when it is not a character or
// its glyph was not identified).
char32_t display_code_char(uint8_t code);
// The display code that shows a character; false when none does.
bool display_code_for(char32_t ch, uint8_t& out);
// EBCDIC (in the given CCSID, "37" by default) to display code. Characters
// the terminal cannot show come out as the substitution code (a blank,
// display code 10).
uint8_t ebcdic_to_display(uint8_t ebcdic, const std::string& ccsid = "37");
// The field attribute byte a terminal's buffer holds for a 3270 field
// attribute (the six data-stream bits under the 11 marker) [documented].
inline uint8_t display_field_attribute(uint8_t fa) { return uint8_t(0xC0 | (fa & 0x3F)); }

} // namespace tn3270
} // namespace em34xx
