#include "machine/display_subtype.hpp"

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

} // namespace tn34xx
