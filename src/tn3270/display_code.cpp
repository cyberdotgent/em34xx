// SPDX-License-Identifier: GPL-3.0-or-later
#include "tn3270/display_code.hpp"

#include "tn5250/codepage.hpp"

namespace em34xx {
namespace tn3270 {

namespace {

// What each display code 00-BF shows on the 3483 [observed: its base
// character set]. 0 = no character or not identified: 00-07 are blank
// positions, 48-4E and the accents on 67-6F could not be told apart at the
// glyph's size (hypothesis for the ones listed there).
constexpr char32_t kChars[0xC0] = {
    // 00
    0, 0, 0, 0, 0, 0, 0, 0, U'>', U'<', U'[', U']', U')', U'(', U'}', U'{',
    // 10
    U' ', U'=', U'\'', U'"', U'/', U'\\', U'|', U'¦', U'?', U'!', U'$', U'¢', U'£', U'¥',
    U'₧', U'¤',
    // 20
    U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7', U'8', U'9', U'ß', U'§', U'#', U'@', U'%', U'_',
    // 30
    U'&', U'-', U'.', U',', U':', U'+', U'¬', U'¯', U'°', 0, U'^', U'~', U'¨', U'`',
    U'´', U'¸',
    // 40
    U'à', U'è', U'ì', U'ò', U'ù', U'ã', U'õ', U'ÿ', 0, 0, 0, 0, 0, 0, 0,
    U'ç',
    // 50
    U'ä', U'ë', U'ï', U'ö', U'ü', U'â', U'ê', U'î', U'ô', U'û',
    U'á', U'é', U'í', U'ó', U'ú', U'ñ',
    // 60
    U'À', U'È', U'Ì', U'Ò', U'Ù', U'Ã', U'Õ', 0, 0, 0, 0, 0, 0, 0, 0,
    U'Ç',
    // 70
    U'Ä', U'Ë', U'Ï', U'Ö', U'Ü', U'Â', U'Ê', U'Î', U'Ô', U'Û',
    U'Á', U'É', U'Í', U'Ó', U'Ú', U'Ñ',
    // 80
    U'a', U'b', U'c', U'd', U'e', U'f', U'g', U'h', U'i', U'j', U'k', U'l', U'm', U'n', U'o', U'p',
    // 90
    U'q', U'r', U's', U't', U'u', U'v', U'w', U'x', U'y', U'z', U'æ', U'ø', U'å', U'ç', 0, 0,
    // A0
    U'A', U'B', U'C', U'D', U'E', U'F', U'G', U'H', U'I', U'J', U'K', U'L', U'M', U'N', U'O', U'P',
    // B0
    U'Q', U'R', U'S', U'T', U'U', U'V', U'W', U'X', U'Y', U'Z', U'Æ', U'Ø', U'Å', U'Ç', U';',
    U'*',
};

} // namespace

char32_t display_code_char(uint8_t code) { return code < 0xC0 ? kChars[code] : 0; }

bool display_code_for(char32_t ch, uint8_t& out) {
    if (ch == 0) return false;
    for (unsigned c = 0; c < 0xC0; ++c)
        if (kChars[c] == ch) {
            out = uint8_t(c);
            return true;
        }
    return false;
}

uint8_t ebcdic_to_display(uint8_t e, const std::string& ccsid) {
    if (e == 0x00) return 0x00;   // null stays null (shows as a blank)
    const tn5250::CodePage* cp = tn5250::CodePage::find(ccsid);
    if (!cp) cp = tn5250::CodePage::find("37");
    uint8_t out = 0x10;
    if (cp && display_code_for(cp->to_unicode(e), out)) return out;
    return 0x10;
}

} // namespace tn3270
} // namespace em34xx
