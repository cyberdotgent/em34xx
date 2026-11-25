// SPDX-License-Identifier: GPL-3.0-or-later
// Screen burn-in (present/burn_in.hpp) and its rendering.
#include "config/settings.hpp"
#include "present/burn_in.hpp"
#include "present/crt_render.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

using namespace em34xx;

namespace {

// A raster as the 3477 presents it: 4:3 glass with unequal borders.
Framebuffer raster(int w, int h) {
    Framebuffer fb;
    fb.resize(w, h);
    fb.display_aspect = 4.0 / 3.0;
    fb.border_x = 0.06;
    fb.border_y = 0.077;
    return fb;
}

void fill(Framebuffer& fb, double u0, double u1, double v0, double v1, uint32_t colour) {
    for (int y = int(v0 * fb.height); y < int(v1 * fb.height); ++y)
        for (int x = int(u0 * fb.width); x < int(u1 * fb.width); ++x) fb.row(y)[x] = colour;
}

double loss(const TubeWear& t, double u, double v) { return kMaxLoss * double(t.depth(u, v)); }

} // namespace

TEST_CASE("burn-in: speed names") {
    BurnSpeed s = BurnSpeed::Off;
    for (const char* name : {"off", "slow", "fast", "hyper"}) {
        REQUIRE(parse_burn_speed(name, s));
        CHECK(std::string(burn_speed_name(s)) == name);
    }
    CHECK_FALSE(parse_burn_speed("glacial", s));
    CHECK(s == BurnSpeed::Hyper);   // left alone
}

TEST_CASE("burn-in: how long each speed takes") {
    Framebuffer fb = raster(64, 32);
    fill(fb, 0, 1, 0, 1, 0xFFFFFFFFu);
    struct Case {
        BurnSpeed speed;
        double visible_s, obvious_s;
    };
    for (const Case& c : {Case{BurnSpeed::Slow, 16 * 3600.0, 230 * 3600.0}, Case{BurnSpeed::Fast, 2 * 3600.0, 29 * 3600.0},
                          Case{BurnSpeed::Hyper, 10.0, 150.0}}) {
        TubeWear t;
        t.expose(fb, c.visible_s, c.speed);
        CHECK(loss(t, 0.5, 0.5) == doctest::Approx(0.02).epsilon(0.05));
        t.expose(fb, c.obvious_s - c.visible_s, c.speed);
        CHECK(loss(t, 0.5, 0.5) == doctest::Approx(0.20).epsilon(0.05));
        // It saturates: the tube never gets worse than kMaxLoss.
        for (int i = 0; i < 20; ++i) t.expose(fb, c.obvious_s * 10, c.speed);
        CHECK(loss(t, 0.5, 0.5) <= kMaxLoss);
        CHECK(loss(t, 0.5, 0.5) > kMaxLoss * 0.99);
    }
    TubeWear off;
    off.expose(fb, 1e9, BurnSpeed::Off);
    CHECK(off.fresh());
    CHECK(off.hours() == 0.0);
}

TEST_CASE("burn-in: wear follows brightness and stays on the glass across formats") {
    TubeWear t;
    // A bright and a half-bright block on a 1280x525 raster.
    Framebuffer a = raster(1280, 525);
    fill(a, 0.25, 0.50, 0.25, 0.50, 0xFFFFFFFFu);
    fill(a, 0.60, 0.80, 0.25, 0.50, 0xFF008000u);   // green gun at half
    t.expose(a, 60.0, BurnSpeed::Hyper);
    const uint64_t v1 = t.version();
    // Glass coordinates of raster points.
    auto gu = [&](double u) { return a.border_x + (1 - 2 * a.border_x) * u; };
    auto gv = [&](double v) { return a.border_y + (1 - 2 * a.border_y) * v; };
    const double bright = t.depth(gu(0.375), gv(0.375)), half = t.depth(gu(0.70), gv(0.375));
    CHECK(bright > 0.2);
    CHECK(half > 0.0);
    CHECK(half < bright * 0.7);
    CHECK(t.depth(gu(0.10), gv(0.10)) == 0.0f);   // never lit
    CHECK(t.depth(0.01, 0.5) == 0.0f);            // the border
    CHECK(t.hours() == doctest::Approx(60.0 / 3600.0));

    // Another model's raster lights the same place of the glass: the wear
    // adds up there, and nowhere else.
    Framebuffer b = raster(960, 400);
    fill(b, 0.25, 0.50, 0.25, 0.50, 0xFFFFFFFFu);
    t.expose(b, 60.0, BurnSpeed::Hyper);
    CHECK(t.depth(gu(0.375), gv(0.375)) > bright);
    CHECK(t.depth(gu(0.10), gv(0.10)) == 0.0f);
    CHECK(t.version() != v1);

    // A new tube.
    const uint64_t v2 = t.version();
    t.reset();
    CHECK(t.fresh());
    CHECK(t.hours() == 0.0);
    CHECK(t.version() != v2);
}

TEST_CASE("burn-in: a slow tube is not redrawn every second") {
    Framebuffer fb = raster(64, 32);
    fill(fb, 0, 1, 0, 1, 0xFFFFFFFFu);
    TubeWear t;
    const uint64_t v = t.version();
    for (int i = 0; i < 600; ++i) t.expose(fb, 1.0, BurnSpeed::Slow);   // ten minutes
    CHECK(t.version() == v);
    CHECK(t.unsaved());
    for (int i = 0; i < 4; ++i) t.expose(fb, 3600.0, BurnSpeed::Slow);
    CHECK(t.version() != v);
}

TEST_CASE("burn-in: the tube file") {
    Framebuffer fb = raster(320, 200);
    fill(fb, 0.1, 0.4, 0.1, 0.4, 0xFFFFFFFFu);
    TubeWear t;
    CHECK_FALSE(t.unsaved());
    t.expose(fb, 30.0, BurnSpeed::Hyper);
    CHECK(t.unsaved());

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "em34xx_burn_test";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "tube.burn").string();
    std::filesystem::remove(path);
    std::string err;
    TubeWear back;
    REQUIRE(load_tube(path, back, err));   // no file: a new tube
    CHECK(back.fresh());
    REQUIRE(save_tube(path, t, err));
    CHECK_FALSE(t.unsaved());
    REQUIRE(load_tube(path, back, err));
    CHECK(back.depth(0.3, 0.3) == t.depth(0.3, 0.3));
    CHECK(back.hours() == t.hours());
    CHECK_FALSE(back.unsaved());

    // Anything else is refused and the tube is left as it was.
    std::string data = t.serialize();
    CHECK_FALSE(back.deserialize(data.substr(0, data.size() - 4), err));
    CHECK_FALSE(back.deserialize("EM34TUBE", err));
    data[0] = 'X';
    CHECK_FALSE(back.deserialize(data, err));
    CHECK(back.depth(0.3, 0.3) == t.depth(0.3, 0.3));
    std::filesystem::remove_all(dir);
}

TEST_CASE("burn-in: worn phosphor is dimmer when lit and darker when not") {
    Framebuffer fb = raster(128, 48);
    fill(fb, 0.0, 0.5, 0.0, 1.0, 0xFFFFFFFFu);   // left half lit
    TubeWear t;
    CrtParams p;
    p.enabled = false;   // sharp: one raster pixel is one level
    const int w = 400, h = 300;
    std::vector<uint32_t> fresh_img, worn_img, plain;
    auto green = [&](const std::vector<uint32_t>& img, int x, int y) { return int((img[size_t(y) * w + size_t(x)] >> 8) & 0xFF); };

    // A fresh tube with burn-in on: full light, and dark grey glass
    // everywhere else, border included.
    CrtRenderer r;
    r.render(fb, w, h, p, fresh_img, &t);
    CHECK(green(fresh_img, 100, 150) == 255);
    const int face = green(fresh_img, 300, 150);
    CHECK(face > 0);
    CHECK(face < 20);
    CHECK(green(fresh_img, 2, 2) == face);
    // Without a tube the glass is black, as before.
    crt_render(fb, w, h, p, plain);
    CHECK(green(plain, 300, 150) == 0);
    CHECK(green(plain, 100, 150) == 255);

    // Nothing changed: nothing redrawn. Wear the left half: all redrawn.
    CHECK(r.render(fb, w, h, p, fresh_img, &t).empty());
    t.expose(fb, 600.0, BurnSpeed::Hyper);
    CrtRenderer::Rows rows = r.render(fb, w, h, p, worn_img, &t);
    CHECK(rows.row1 - rows.row0 == h);
    CHECK(green(worn_img, 100, 150) < 240);          // lit, but worn
    CHECK(green(worn_img, 100, 150) > 150);
    CHECK(green(worn_img, 300, 150) == face);        // the right half is fresh

    // The same wear on a dark screen: the ghost is darker than fresh glass.
    Framebuffer dark = raster(128, 48);
    crt_render(dark, w, h, p, worn_img, &t);
    CHECK(green(worn_img, 100, 150) < face);
    CHECK(green(worn_img, 300, 150) == face);
}
