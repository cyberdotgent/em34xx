#include "config/config.hpp"
#include "machine/display_profile.hpp"

#include <doctest/doctest.h>

using namespace tn34xx;

TEST_CASE("display subtype names and shorthands") {
    DisplaySubtype s;
    CHECK(parse_display_subtype("Amber", s));
    CHECK(s == DisplaySubtype::Amber);
    CHECK(parse_display_subtype("paper-white", s));
    CHECK(s == DisplaySubtype::PaperWhite);
    CHECK(parse_display_subtype("white", s));
    CHECK(s == DisplaySubtype::PaperWhite);
    CHECK(parse_display_subtype("colour", s));
    CHECK(s == DisplaySubtype::Color);
    CHECK_FALSE(parse_display_subtype("plasma", s));
    CHECK(is_monochrome(DisplaySubtype::Green));
    CHECK_FALSE(is_monochrome(DisplaySubtype::Color));
}

TEST_CASE("monochrome rendering maps luminance onto the phosphor") {
    Framebuffer fb;
    fb.resize(3, 1);
    fb.pixels = {0xFFFFFFFFu, 0xFF000000u, 0xFFFF0000u};
    Framebuffer colour = fb;
    apply_display_subtype(DisplaySubtype::Color, colour);
    CHECK(colour.pixels == fb.pixels);

    apply_display_subtype(DisplaySubtype::Amber, fb);
    CHECK(fb.pixels[0] == phosphor_color(DisplaySubtype::Amber));  // full white -> full phosphor
    CHECK(fb.pixels[1] == 0xFF000000u);                            // black stays black
    // Pure red has luma ~30%: dimmer amber, same hue.
    const uint32_t r = (fb.pixels[2] >> 16) & 0xFF, g = (fb.pixels[2] >> 8) & 0xFF, b = fb.pixels[2] & 0xFF;
    CHECK(r > 60);
    CHECK(r < 100);
    CHECK(g < r);
    CHECK(b == 0);
}

TEST_CASE("display and keyboard options from the command line") {
    AppConfig cfg;
    std::string err;
    const char* argv[] = {"tn34xx", "--amber", "--keyboard", "enhanced", "--keyboard-layout", "be", "--keyboard-id", "7f"};
    REQUIRE(parse_command_line(8, const_cast<char**>(argv), cfg, err));
    CHECK(cfg.terminal.display == DisplaySubtype::Amber);
    CHECK(cfg.terminal.keyboard_type == "enhanced");
    CHECK(cfg.terminal.keyboard_layout == "be");
    CHECK(cfg.terminal.keyboard_id == 0x7F);

    AppConfig c2;
    const char* argv2[] = {"tn34xx", "--display", "colour"};
    REQUIRE(parse_command_line(3, const_cast<char**>(argv2), c2, err));
    CHECK(c2.terminal.display == DisplaySubtype::Color);
}
