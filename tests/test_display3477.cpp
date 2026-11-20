#include "devices/i2c.hpp"
#include "devices/ibm3477/display.hpp"

#include <doctest/doctest.h>

#include <vector>

using namespace tn34xx;

namespace {

constexpr uint32_t kGrey = 0xFFD0D0D0u;     // normal intensity
constexpr uint32_t kBright = 0xFFFFFFFFu;   // high intensity
constexpr uint32_t kBlack = 0xFF000000u;

// A bus with the 3477 RAM (0-1FFFF) and the display controller page.
struct Rig {
    Bus bus{20};
    TerminalOptions opts;
    Ibm3477Display disp;

    explicit Rig(DisplaySubtype subtype = DisplaySubtype::Green)
        : opts(make_opts(subtype)), disp(bus, opts) {
        bus.map_ram("ram", 0, 0x20000);
        bus.map_mmio("display", 0x20100, 0x100, &disp);
    }
    static TerminalOptions make_opts(DisplaySubtype s) {
        TerminalOptions o;
        o.display = s;
        return o;
    }
    void reg(uint32_t off, uint8_t v) { bus.mem_write8(0x20100 + off, v); }
    uint8_t status() { return bus.mem_read8(0x2012F); }

    // 80 x 24+1 rows of 12x21 cells, main window at C200, status at EE00.
    void mode_80x24_12px() {
        reg(0x0F, 20);     // 21 scan lines
        reg(0x11, 18);     // underline/cursor scan lines 18-19
        reg(0x12, 19);
        reg(0x14, 25);     // rows including the status line
        reg(0x15, 24);
        reg(0x25, 0x00);
        reg(0x2A, 0x01);   // 12-pixel cells
        reg(0x30, 0x00);
        reg(0x31, 0x42);
        reg(0x34, 0x00);
        reg(0x35, 0x6E);
        reg(0x28, 0x10);   // video on
    }
    void cell(uint32_t base, int row, int col, uint8_t ch, uint8_t attr, int cols = 80) {
        const uint32_t a = base + uint32_t(row * cols + col) * 3;
        bus.poke8(a, ch);
        bus.poke8(a + 1, attr);
        bus.poke8(a + 2, 0);
    }
    // Made-up glyph: line 0 = 1111 0000 | 1010 0101, line 1 = 1000 0000 | 0000 0000.
    void glyph(uint32_t plane, uint8_t code) {
        bus.poke8(plane + code * 32u, 0xF0);
        bus.poke8(plane + 0x2000 + code * 32u, 0xA5);
        bus.poke8(plane + code * 32u + 1, 0x80);
    }
    void blank_screen(uint32_t base, int cells) {
        for (int i = 0; i < cells; ++i) {
            bus.poke8(base + uint32_t(i) * 3, 0x40);
            bus.poke8(base + uint32_t(i) * 3 + 1, 0x20);
            bus.poke8(base + uint32_t(i) * 3 + 2, 0);
        }
    }
};

uint32_t px(const Framebuffer& fb, int x, int y) { return fb.pixels[size_t(y) * size_t(fb.width) + size_t(x)]; }

} // namespace

TEST_CASE("3477 display: blank until enabled") {
    Rig r;
    Framebuffer fb;
    r.disp.render(fb);
    CHECK(fb.width > 0);
    for (uint32_t p : fb.pixels) CHECK(p == kBlack);
    r.mode_80x24_12px();
    r.reg(0x28, 0x00);   // video off while the font is reloaded
    r.glyph(0x18000, 0x40);
    r.blank_screen(0xC200, 80 * 24);
    r.disp.render(fb);
    for (uint32_t p : fb.pixels) CHECK(p == kBlack);
}

TEST_CASE("3477 display: glyphs come from character-generator RAM") {
    Rig r;
    r.mode_80x24_12px();
    r.glyph(0x18000, 0xC1);
    r.cell(0xC200, 1, 2, 0xC1, 0x20);
    Framebuffer fb;
    r.disp.render(fb);
    REQUIRE(fb.width == 80 * 12);
    REQUIRE(fb.height == 25 * 21);
    const int x0 = 2 * 12, y0 = 1 * 21;
    // Line 0: plane 1 = F0, extension high nibble = A (12-pixel cells).
    const bool expect0[12] = {1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0};
    for (int x = 0; x < 12; ++x) CHECK(px(fb, x0 + x, y0) == (expect0[x] ? kGrey : kBlack));
    CHECK(px(fb, x0, y0 + 1) == kGrey);
    CHECK(px(fb, x0 + 1, y0 + 1) == kBlack);
    CHECK(px(fb, x0, y0 + 2) == kBlack);
    // The neighbouring cell (code 00, empty glyph) stays dark.
    CHECK(px(fb, x0 + 12, y0) == kBlack);

    // Changing RAM changes the picture: the emulator holds no font.
    r.bus.poke8(0x18000 + 0xC1 * 32, 0x0F);
    r.disp.render(fb);
    CHECK(px(fb, x0, y0) == kBlack);
    CHECK(px(fb, x0 + 7, y0) == kGrey);
}

TEST_CASE("3477 display: cell widths 16 and 8") {
    Rig r;
    r.mode_80x24_12px();
    r.glyph(0x18000, 0xC1);
    r.cell(0xC200, 0, 0, 0xC1, 0x20);
    Framebuffer fb;

    r.reg(0x2A, 0x00);   // 16-pixel cells: all 8 extension columns
    r.disp.render(fb);
    CHECK(fb.width == 80 * 16);
    const bool expect16[16] = {1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1};
    for (int x = 0; x < 16; ++x) CHECK(px(fb, x, 0) == (expect16[x] ? kGrey : kBlack));

    r.reg(0x25, 0x10);   // 132 columns of 8-pixel cells, no extension plane
    r.reg(0x0F, 17);
    r.disp.render(fb);
    CHECK(fb.width == 132 * 8);
    CHECK(fb.height == 25 * 18);
    for (int x = 0; x < 8; ++x) CHECK(px(fb, x, 0) == (x < 4 ? kGrey : kBlack));
    CHECK(px(fb, 8, 0) == kBlack);
}

TEST_CASE("3477 display: attributes, font 2 and the status line") {
    Rig r;
    r.mode_80x24_12px();
    r.glyph(0x18000, 0xC1);
    r.bus.poke8(0x1C000 + 0x05 * 32, 0xFF);   // font 2 glyph 05: line 0 solid
    r.cell(0xC200, 0, 0, 0xC1, 0x22);           // high intensity
    r.cell(0xC200, 0, 1, 0xC1, 0x21);           // reverse
    r.cell(0xC200, 0, 2, 0xC1, 0x27);           // non-display
    r.cell(0xC200, 0, 3, 0xC1, 0x24);           // underline
    r.cell(0xC200, 0, 4, 0x05, 0xA0);           // font 2
    r.cell(0xEE00, 0, 0, 0xC1, 0x00);           // status line
    Framebuffer fb;
    r.disp.render(fb);
    CHECK(px(fb, 0, 0) == kBright);
    CHECK(px(fb, 12 + 0, 0) == kBlack);   // reversed: lit pixel becomes dark
    CHECK(px(fb, 12 + 5, 0) == kGrey);    // and dark pixels lit
    CHECK(px(fb, 12 + 5, 10) == kGrey);
    for (int x = 0; x < 12; ++x) CHECK(px(fb, 24 + x, 0) == kBlack);
    CHECK(px(fb, 36 + 6, 18) == kGrey);   // underline on its scan lines
    CHECK(px(fb, 36 + 6, 19) == kGrey);
    CHECK(px(fb, 36 + 6, 17) == kBlack);
    CHECK(px(fb, 48 + 6, 0) == kGrey);    // font 2 glyph
    CHECK(px(fb, 48 + 6, 1) == kBlack);
    CHECK(px(fb, 0, 24 * 21) == kGrey);   // status line row from its own start address
    CHECK(px(fb, 5, 24 * 21) == kBlack);

    // Rule above the status line: drawn on the underline scan lines (18-19)
    // of the last main row, as photographed on a real tube.
    CHECK(px(fb, 30, 23 * 21 + 18) == kBlack);
    r.reg(0x28, 0x50);
    r.disp.render(fb);
    CHECK(px(fb, 30, 23 * 21 + 18) == kGrey);
    CHECK(px(fb, 30, 23 * 21 + 19) == kGrey);
    CHECK(px(fb, 30, 23 * 21 + 17) == kBlack);
    CHECK(px(fb, 30, 24 * 21) == kBlack);   // not on the status row itself
}

TEST_CASE("3477 display: colour attributes") {
    Rig r(DisplaySubtype::Color);
    r.mode_80x24_12px();
    r.bus.poke8(0x18000 + 0xC1 * 32, 0xFF);
    const uint8_t attrs[] = {0x20, 0x22, 0x28, 0x30, 0x32, 0x38, 0x3A};
    for (int i = 0; i < 7; ++i) r.cell(0xC200, 0, i, 0xC1, attrs[i]);
    Framebuffer fb;
    r.disp.render(fb);
    std::vector<uint32_t> seen;
    for (int i = 0; i < 7; ++i) {
        const uint32_t c = px(fb, i * 12 + 3, 0);
        CHECK(c != kBlack);
        for (uint32_t s : seen) CHECK(c != s);   // seven distinct colours
        seen.push_back(c);
    }
    const uint32_t green = seen[0], red = seen[2];
    CHECK(((green >> 8) & 0xFF) > ((green >> 16) & 0xFF));
    CHECK(((red >> 16) & 0xFF) > ((red >> 8) & 0xFF));
}

TEST_CASE("3477 display: blink and cursor") {
    Rig r;
    r.mode_80x24_12px();
    r.bus.poke8(0x18000 + 0xC1 * 32, 0xFF);
    r.cell(0xC200, 0, 0, 0xC1, 0x28);   // blink
    Framebuffer fb;
    r.disp.render(fb);
    const uint32_t first = px(fb, 3, 0);
    bool changed = false;
    for (int f = 0; f < 80 && !changed; ++f) {
        r.disp.tick(r.disp.cycles_per_frame());
        r.disp.render(fb);
        changed = px(fb, 3, 0) != first;
    }
    CHECK(changed);

    // Block cursor at row 2, column 5 when 20128 bit 7 is set.
    r.reg(0x17, 5);
    r.reg(0x18, 2);
    r.disp.render(fb);
    CHECK(px(fb, 5 * 12 + 3, 2 * 21 + 10) == kBlack);
    r.reg(0x28, 0x90);
    r.disp.render(fb);
    CHECK(px(fb, 5 * 12 + 3, 2 * 21 + 10) == kGrey);
    CHECK(px(fb, 6 * 12 + 3, 2 * 21 + 10) == kBlack);
}

TEST_CASE("3477 display: vertical retrace toggles once per frame") {
    Rig r;
    r.disp.set_clock_hz(8000000);
    const uint64_t frame = r.disp.cycles_per_frame();
    CHECK(frame == 8000000 / 60);
    int rises = 0, falls = 0;
    bool last = r.status() & 0x08;
    for (uint64_t c = 0; c < frame * 10 - frame / 2; c += 50) {
        r.disp.tick(50);
        const bool now = r.status() & 0x08;
        if (now && !last) ++rises;
        if (!now && last) ++falls;
        last = now;
    }
    CHECK(rises == 10);
    CHECK(falls == 10);
    CHECK(r.disp.frame_count() == 9);
}

TEST_CASE("3477 display: block request interrupt and acknowledge") {
    Rig r;
    std::vector<bool> edges;
    r.disp.set_irq([&](bool on) { edges.push_back(on); });
    const uint64_t frame = r.disp.cycles_per_frame();
    for (int i = 0; i < 3; ++i) r.disp.tick(frame / 2);
    CHECK(edges.empty());   // no request, no interrupt

    r.reg(0x3B, 0x43);
    r.reg(0x3A, 0xFF);
    r.reg(0x39, 0x42);
    r.reg(0x38, 0x00);
    for (int i = 0; i < 4 && edges.empty(); ++i) r.disp.tick(frame / 2);
    REQUIRE(edges.size() == 1);
    CHECK(edges[0]);
    CHECK(r.disp.irq_pending());
    r.disp.tick(frame);   // stays asserted until acknowledged
    CHECK(edges.size() == 1);
    r.reg(0x36, 0xFF);
    REQUIRE(edges.size() == 2);
    CHECK_FALSE(edges[1]);
    CHECK_FALSE(r.disp.irq_pending());
}

namespace {

class FakeI2c : public I2cDevice {
public:
    bool i2c_select(uint8_t addr7, bool read) override {
        selected = addr7;
        reading = read;
        return true;
    }
    bool i2c_write(uint8_t b) override {
        written.push_back(b);
        return true;
    }
    uint8_t i2c_read() override { return 0x5A; }
    int selected = -1;
    bool reading = false;
    std::vector<uint8_t> written;
};

// Bit-bang through 2012F: bit0 pulls SDA low, bit1 pulls SCL low.
struct Master {
    Rig& r;
    bool scl_low = false, sda_low = false;
    void put() { r.reg(0x2F, uint8_t((sda_low ? 1 : 0) | (scl_low ? 2 : 0))); }
    void sda(bool high) { sda_low = !high; put(); }
    void scl(bool high) { scl_low = !high; put(); }
    void start() { sda(true); scl(true); sda(false); scl(false); }
    void stop() { sda(false); scl(true); sda(true); }
    bool byte_out(uint8_t b) {
        for (int i = 7; i >= 0; --i) {
            sda((b >> i) & 1);
            scl(true);
            scl(false);
        }
        sda(true);
        scl(true);
        const bool ack = r.status() & 0x01;   // SDA held low by the slave
        scl(false);
        return ack;
    }
    uint8_t byte_in() {
        sda(true);
        uint8_t v = 0;
        for (int i = 0; i < 8; ++i) {
            scl(true);
            v = uint8_t((v << 1) | ((r.status() & 0x01) ? 0 : 1));
            scl(false);
        }
        sda(true);   // NACK
        scl(true);
        scl(false);
        return v;
    }
};

} // namespace

TEST_CASE("3477 display: I2C pass-through on 2012F") {
    Rig r;
    I2cBus bus;
    FakeI2c dev;
    bus.attach(0x50, &dev);
    r.disp.set_i2c(&bus);

    r.reg(0x2F, 0x00);
    CHECK((r.status() & 0x03) == 0);   // both lines released read as 0
    r.reg(0x2F, 0x03);
    CHECK((r.status() & 0x03) == 0x03);

    Master m{r};
    m.scl(true);
    m.sda(true);
    m.start();
    CHECK(m.byte_out(0xA0));
    CHECK(dev.selected == 0x50);
    CHECK_FALSE(dev.reading);
    CHECK(m.byte_out(0x1B));
    m.stop();
    REQUIRE(dev.written.size() == 1);
    CHECK(dev.written[0] == 0x1B);

    m.start();
    CHECK(m.byte_out(0xA1));
    CHECK(dev.reading);
    CHECK(m.byte_in() == 0x5A);
    m.stop();
    CHECK((r.status() & 0x03) == 0);
}
