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

TEST_CASE("keyboard ID resolution") {
    TerminalOptions t;
    uint8_t a = 0, b = 0;
    std::string err;
    REQUIRE(resolve_keyboard_id(t, a, b, err));   // 122-key US default
    CHECK(a == 0xBF);
    CHECK(b == 0xBF);
    t.keyboard_type = "enhanced";
    REQUIRE(resolve_keyboard_id(t, a, b, err));
    CHECK(a == 0x7F);
    CHECK(b == 0x7F);
    t.keyboard_layout = "nosuch";
    CHECK_FALSE(resolve_keyboard_id(t, a, b, err));
    t.keyboard_id = 0x9F;                          // explicit override wins
    REQUIRE(resolve_keyboard_id(t, a, b, err));
    CHECK(b == 0x9F);
    t.keyboard_type = "qwerty";
    CHECK_FALSE(resolve_keyboard_id(t, a, b, err));
}

TEST_CASE("framebuffer presentation follows the tube's aspect ratio") {
    Framebuffer fb;
    fb.resize(1280, 525);
    CHECK(fb.presented_height() == 525);   // square pixels by default
    fb.display_aspect = 4.0 / 3.0;
    CHECK(fb.presented_height() == 960);
    fb.resize(960, 525);
    CHECK(fb.presented_height() == 720);
    fb.resize(1280, 525);
    fb.border = 0.025;                      // 2.5% black margin on each side
    CHECK(fb.presented_width() == 1347);
    CHECK(fb.presented_height() == 1010);
}
