// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/config.hpp"
#include "present/crt_render.hpp"
#include "present/display_profile.hpp"

#include <doctest/doctest.h>

using namespace em34xx;

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
    const char* argv[] = {"em34xx", "--amber", "--keyboard", "enhanced", "--keyboard-layout", "be", "--keyboard-id", "7f"};
    REQUIRE(parse_command_line(8, const_cast<char**>(argv), cfg, err));
    CHECK(cfg.terminal.display == DisplaySubtype::Amber);
    CHECK(cfg.terminal.keyboard_type == "enhanced");
    CHECK(cfg.terminal.keyboard_layout == "be");
    CHECK(cfg.terminal.keyboard_id == 0x7F);

    AppConfig c2;
    const char* argv2[] = {"em34xx", "--display", "colour"};
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
    fb.border_x = 0.06;                     // unequal margins as on a real tube
    fb.border_y = 0.077;
    CHECK(fb.presented_width() == 1455);
    CHECK(fb.presented_height() == 1091);
}

TEST_CASE("CRT rendering: scan lines, border and aspect") {
    Framebuffer fb;
    fb.resize(8, 4);
    fb.display_aspect = 4.0 / 3.0;
    fb.border_x = 0.1;
    fb.border_y = 0.1;
    for (int x = 0; x < 8; ++x) fb.row(1)[x] = 0xFFFFFFFFu;   // one lit raster line
    int w = 0, h = 0;
    crt_fit(fb, 1000, 1000, w, h);
    CHECK(w == 1000);
    CHECK(h == 750);
    std::vector<uint32_t> out;
    CrtParams p;
    crt_render(fb, 400, 300, p, out);
    REQUIRE(out.size() == size_t(400 * 300));
    auto g = [&](int x, int y) { return int((out[size_t(y) * 400 + size_t(x)] >> 8) & 0xFF); };
    // Raster spans y 30..270 (60 output px per line); line 1 centre at y 120.
    CHECK(g(200, 120) > 240);   // beam centre at full brightness
    CHECK(g(200, 150) * 2 < g(200, 120));   // gap at less than half the line centre
    CHECK(g(200, 60) < 20);     // unlit line stays dark
    CHECK(g(10, 120) == 0);     // left border is black
    p.enabled = false;          // sharp mode: no scan-line gaps
    crt_render(fb, 400, 300, p, out);
    CHECK(g(200, 140) > 240);
}

TEST_CASE("CRT rendering: the incremental renderer matches a full render") {
    Framebuffer fb;
    fb.resize(64, 24);
    fb.display_aspect = 4.0 / 3.0;
    fb.border_x = 0.05;
    fb.border_y = 0.07;
    uint32_t seed = 12345;
    auto rnd = [&] { seed = seed * 1103515245u + 12345u; return seed >> 8; };
    for (uint32_t& px : fb.pixels) px = (rnd() & 3) ? 0xFF000000u : (0xFF000000u | (rnd() & 0xFFFFFFu));
    for (const bool enabled : {true, false}) {
        CrtParams p;
        p.enabled = enabled;
        for (const int h : {90, 300}) {   // beam widened (lines < 1 px apart) and not
            const int w = h * 4 / 3;
            CrtRenderer r;
            std::vector<uint32_t> inc, full;
            CrtRenderer::Rows rows = r.render(fb, w, h, p, inc);
            CHECK(rows.row0 == 0);
            CHECK(rows.row1 == h);
            crt_render(fb, w, h, p, full);
            CHECK(inc == full);
            // Nothing changed: nothing redrawn.
            CHECK(r.render(fb, w, h, p, inc).empty());
            // Change one raster line: only the rows it reaches are redrawn,
            // and the result is still that of a full render.
            Framebuffer fb2 = fb;
            for (int x = 10; x < 20; ++x) fb2.row(12)[x] = 0xFF00FF00u;
            rows = r.render(fb2, w, h, p, inc);
            CHECK_FALSE(rows.empty());
            CHECK(rows.row0 > 0);
            CHECK(rows.row1 < h);
            crt_render(fb2, w, h, p, full);
            CHECK(inc == full);
            // A new size redraws everything.
            rows = r.render(fb2, w + 10, h + 8, p, inc);
            CHECK(rows.row1 - rows.row0 == h + 8);
            crt_render(fb2, w + 10, h + 8, p, full);
            CHECK(inc == full);
        }
    }
}
