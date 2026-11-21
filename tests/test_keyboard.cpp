// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices/ibm3477/keyboard_if.hpp"
#include "input/keyboard_ids.hpp"
#include "input/set3_keyboard.hpp"

#include <doctest/doctest.h>

#include <vector>

using namespace em34xx;

namespace {

std::vector<uint8_t> drain(Set3Keyboard& k) {
    std::vector<uint8_t> v;
    while (k.has_output()) v.push_back(k.read_output());
    return v;
}

using Bytes = std::vector<uint8_t>;

void press(Set3Keyboard& k, uint32_t hid, bool down) {
    HostKeyEvent ev;
    ev.hid_usage = hid;
    ev.pressed = down;
    k.host_key(ev);
}

} // namespace

TEST_CASE("keyboard type parsing") {
    KeyboardType t = KeyboardType::Enhanced;
    CHECK(parse_keyboard_type("122", t));
    CHECK(t == KeyboardType::Terminal122);
    CHECK(parse_keyboard_type("Terminal", t));
    CHECK(t == KeyboardType::Terminal122);
    CHECK(parse_keyboard_type("enhanced", t));
    CHECK(t == KeyboardType::Enhanced);
    CHECK(parse_keyboard_type("101", t));
    CHECK(parse_keyboard_type("102", t));
    CHECK(t == KeyboardType::Enhanced);
    CHECK_FALSE(parse_keyboard_type("84", t));
    CHECK(std::string(keyboard_type_name(KeyboardType::Terminal122)) == "122");
    CHECK(std::string(keyboard_type_name(KeyboardType::Enhanced)) == "enhanced");
}

TEST_CASE("keyboard ID table") {
    uint8_t a = 0, b = 0;
    REQUIRE(keyboard_id_bytes(KeyboardType::Terminal122, "us", a, b));
    CHECK(a == 0xBF);
    CHECK(b == 0xBF);   // 10111111
    REQUIRE(keyboard_id_bytes(KeyboardType::Enhanced, "us", a, b));
    CHECK(a == 0x7F);
    CHECK(b == 0x7F);   // 01111111
    REQUIRE(keyboard_id_bytes(KeyboardType::Terminal122, "de", a, b));
    CHECK(b == 0xBE);
    REQUIRE(keyboard_id_bytes(KeyboardType::Enhanced, "UK", a, b));
    CHECK(b == 0x6C);
    REQUIRE(keyboard_id_bytes(KeyboardType::Enhanced, "be", a, b));
    CHECK(b == 0x7D);
    REQUIRE(keyboard_id_bytes(KeyboardType::Terminal122, "fi", a, b));  // alias of se
    CHECK(b == 0xAF);
    REQUIRE(keyboard_id_bytes(KeyboardType::Enhanced, "hu", a, b));
    CHECK(b == 0x41);
    // No 122-key Hungarian keyboard, no Enhanced data-entry keyboard.
    CHECK_FALSE(keyboard_id_bytes(KeyboardType::Terminal122, "hu", a, b));
    CHECK_FALSE(keyboard_id_bytes(KeyboardType::Enhanced, "us-dataentry", a, b));
    CHECK_FALSE(keyboard_id_bytes(KeyboardType::Enhanced, "xx", a, b));

    // Names are unique, lowercase, and every pair follows the type rule.
    const auto& ls = keyboard_layouts();
    CHECK(ls.size() >= 30);
    for (size_t i = 0; i < ls.size(); ++i) {
        for (const char* p = ls[i].name; *p; ++p) CHECK_FALSE((*p >= 'A' && *p <= 'Z'));
        for (size_t j = i + 1; j < ls.size(); ++j) CHECK(std::string(ls[i].name) != ls[j].name);
        if (ls[i].id122) CHECK((ls[i].id122 & 0xC0) == 0x80);
        if (ls[i].id_enhanced) CHECK((ls[i].id_enhanced & 0xC0) == 0x40);
        if (ls[i].id122 && ls[i].id_enhanced) CHECK(ls[i].id122 == (ls[i].id_enhanced ^ 0xC0));
    }
}

TEST_CASE("reset and BAT") {
    Set3Keyboard k(KeyboardType::Terminal122, 0xBF, 0xBE);
    k.power_on();
    CHECK(drain(k) == Bytes{0xAA, 0xBF, 0xBE});

    k.set_bat_sends_id(false);
    k.host_write(0xFF);
    CHECK(drain(k) == Bytes{0xFA, 0xAA});

    Set3Keyboard e(KeyboardType::Enhanced, 0x7F, 0x7F);
    e.power_on();
    CHECK(drain(e) == Bytes{0xAA});
}

TEST_CASE("read ID, echo, resend, unknown command") {
    Set3Keyboard k(KeyboardType::Enhanced, 0x7F, 0x6C);
    k.power_on();
    drain(k);
    k.host_write(0xF2);
    CHECK(drain(k) == Bytes{0xFA, 0x7F, 0x6C});
    k.host_write(0xEE);
    CHECK(drain(k) == Bytes{0xEE});
    k.host_write(0xFE);
    CHECK(drain(k) == Bytes{0xEE});
    k.host_write(0xEF);
    CHECK(drain(k) == Bytes{0xFE});
}

TEST_CASE("scan-code set select") {
    Set3Keyboard k;
    k.power_on();
    drain(k);
    k.host_write(0xF0);
    k.host_write(0x00);
    CHECK(drain(k) == Bytes{0xFA, 0xFA, 0x03});
    k.host_write(0xF0);
    k.host_write(0x03);
    CHECK(drain(k) == Bytes{0xFA, 0xFA});
    k.host_write(0xF0);
    k.host_write(0x07);
    CHECK(drain(k) == Bytes{0xFA, 0xFE});
}

TEST_CASE("set-3 default make/break") {
    Set3Keyboard k(KeyboardType::Enhanced, 0x7F, 0x7F);
    k.power_on();
    drain(k);
    // A letter is typematic make-only: no break code.
    press(k, 0x04, true);   // a
    press(k, 0x04, false);
    CHECK(drain(k) == Bytes{0x1C});
    // Shift is make/break.
    press(k, 0xE1, true);
    press(k, 0xE1, false);
    CHECK(drain(k) == Bytes{0x12, 0xF0, 0x12});
    // Esc on the Enhanced keyboard.
    press(k, 0x29, true);
    CHECK(drain(k) == Bytes{0x08});
    CHECK(Set3Keyboard::default_key_mode(0x14) == Set3Keyboard::MakeBreak);
    CHECK(Set3Keyboard::default_key_mode(0x07) == Set3Keyboard::MakeOnly);
}

TEST_CASE("122-key host mapping") {
    CHECK(Set3Keyboard::hid_to_set3(KeyboardType::Terminal122, 0x68) == 0x08);  // F13
    CHECK(Set3Keyboard::hid_to_set3(KeyboardType::Terminal122, 0x73) == 0x5F);  // F24
    CHECK(Set3Keyboard::hid_to_set3(KeyboardType::Terminal122, 0x29) == 0x05);  // Esc
    CHECK(Set3Keyboard::hid_to_set3(KeyboardType::Terminal122, 0x3A) == 0x07);  // F1
    CHECK(Set3Keyboard::hid_to_set3(KeyboardType::Enhanced, 0x29) == 0x08);
    CHECK(Set3Keyboard::hid_to_set3(KeyboardType::Enhanced, 0x68) == 0x00);
    CHECK(Set3Keyboard::hid_to_set3(KeyboardType::Enhanced, 0x31) == 0x5C);
    CHECK(Set3Keyboard::hid_to_set3(KeyboardType::Terminal122, 0x31) == 0x53);
    CHECK(Set3Keyboard::hid_to_set3(KeyboardType::Enhanced, 0x1000) == 0x00);
}

TEST_CASE("FA on the Enhanced keyboard sets all keys") {
    Set3Keyboard k(KeyboardType::Enhanced, 0x7F, 0x7F);
    k.set_bat_sends_id(false);
    k.power_on();
    drain(k);
    k.host_write(0xFA);
    CHECK(drain(k) == Bytes{0xFA});
    CHECK(k.key_mode(0x21) == Set3Keyboard::TypematicMakeBreak);
}

TEST_CASE("key mode commands") {
    Set3Keyboard k;
    k.set_bat_sends_id(false);
    k.power_on();
    drain(k);

    k.host_write(0xF8);  // all make/break
    CHECK(drain(k) == Bytes{0xFA});
    press(k, 0x04, true);
    press(k, 0x04, false);
    CHECK(drain(k) == Bytes{0x1C, 0xF0, 0x1C});

    k.host_write(0xF9);  // all make-only
    press(k, 0xE1, true);
    press(k, 0xE1, false);
    CHECK(drain(k) == Bytes{0xFA, 0x12});

    // FC <codes> : those keys make/break; the list ends at the next command.
    k.host_write(0xFC);
    k.host_write(0x1C);
    k.host_write(0x32);
    k.host_write(0xF4);
    CHECK(drain(k) == Bytes{0xFA, 0xFA, 0xFA, 0xFA});
    CHECK(k.key_mode(0x1C) == Set3Keyboard::MakeBreak);
    CHECK(k.key_mode(0x32) == Set3Keyboard::MakeBreak);
    CHECK(k.key_mode(0x21) == Set3Keyboard::MakeOnly);
    press(k, 0x05, true);   // b
    press(k, 0x05, false);
    press(k, 0x06, true);   // c
    press(k, 0x06, false);
    CHECK(drain(k) == Bytes{0x32, 0xF0, 0x32, 0x21});

    // 122-key terminal keyboard: FA behaves as FB (key list, typematic,
    // no break code).
    k.host_write(0xFA);
    k.host_write(0x61);
    k.host_write(0x6A);
    CHECK(drain(k) == Bytes{0xFA, 0xFA, 0xFA});
    CHECK(k.key_mode(0x61) == Set3Keyboard::Typematic);
    CHECK(k.key_mode(0x6A) == Set3Keyboard::Typematic);
    CHECK(k.key_mode(0x21) == Set3Keyboard::MakeOnly);
    k.host_write(0xF4);
    drain(k);
    press(k, 0x50, true);   // cursor left (set 3 61): make only, no F0 61
    press(k, 0x50, false);
    CHECK(drain(k) == Bytes{0x61});
    k.host_write(0xF7);
    CHECK(k.key_mode(0x21) == Set3Keyboard::Typematic);
    k.host_write(0xF6);  // defaults
    CHECK(k.key_mode(0x12) == Set3Keyboard::MakeBreak);
    CHECK(k.key_mode(0x1C) == Set3Keyboard::Typematic);
    drain(k);

    // F5 disables scanning; F4 re-enables.
    k.host_write(0xF5);
    press(k, 0x04, true);
    press(k, 0x04, false);
    CHECK(drain(k) == Bytes{0xFA});
    k.host_write(0xF4);
    press(k, 0x04, true);
    CHECK(drain(k) == Bytes{0xFA, 0x1C});
}

TEST_CASE("typematic repeat") {
    Set3Keyboard k;
    k.set_bat_sends_id(false);
    k.power_on();
    drain(k);
    k.host_write(0xF3);
    k.host_write(0x00);  // 30 cps (33.4 ms), 250 ms delay
    CHECK(drain(k) == Bytes{0xFA, 0xFA});
    press(k, 0x04, true);
    press(k, 0x04, true);  // host auto-repeat is ignored
    CHECK(drain(k) == Bytes{0x1C});
    k.advance_us(249000);
    CHECK_FALSE(k.has_output());
    k.advance_us(1000);
    CHECK(drain(k) == Bytes{0x1C});
    k.advance_us(33360 * 2);
    CHECK(drain(k) == Bytes{0x1C, 0x1C});
    press(k, 0x04, false);
    k.advance_us(1000000);
    CHECK_FALSE(k.has_output());
    // Make-only keys do not repeat.
    press(k, 0x3A, true);   // F1
    k.advance_us(1000000);
    CHECK(drain(k) == Bytes{0x07});
}

TEST_CASE("LEDs") {
    Set3Keyboard k;
    k.power_on();
    drain(k);
    k.host_write(0xED);
    k.host_write(0x05);
    CHECK(drain(k) == Bytes{0xFA, 0xFA});
    CHECK(k.scroll_lock_led());
    CHECK_FALSE(k.num_lock_led());
    CHECK(k.caps_lock_led());
    k.host_write(0xED);
    k.host_write(0x08);  // out of range
    CHECK(drain(k) == Bytes{0xFA, 0xFE});
    CHECK(k.leds() == 0x05);
    k.host_write(0x02);
    CHECK(drain(k) == Bytes{0xFA});
    CHECK(k.leds() == 0x02);
}

TEST_CASE("buffer overrun") {
    Set3Keyboard k;
    k.set_bat_sends_id(false);
    k.power_on();
    drain(k);
    for (uint8_t i = 0; i < 20; ++i) {
        k.key(uint8_t(0x15 + i), true);
    }
    auto out = drain(k);
    REQUIRE(out.size() == Set3Keyboard::kBufferLimit + 1);
    CHECK(out.back() == 0x00);
}

TEST_CASE("Shift+F1..F12 enters F13..F24 on the 122-key keyboard") {
    Set3Keyboard k(KeyboardType::Terminal122, 0xBF, 0xBF);
    k.power_on();
    drain(k);
    // Make F13 (08) make/break so that its break code is visible.
    k.host_write(0xFC);
    k.host_write(0x08);
    k.host_write(0xF4);
    drain(k);

    press(k, 0xE1, true);   // Left Shift
    CHECK(drain(k) == Bytes{0x12});
    press(k, 0x3A, true);   // F1
    CHECK(drain(k) == Bytes{0xF0, 0x12, 0x08});
    press(k, 0x3A, false);
    CHECK(drain(k) == Bytes{0xF0, 0x08, 0x12});
    press(k, 0xE1, false);
    CHECK(drain(k) == Bytes{0xF0, 0x12});

    // Shift+F12 -> F24 (5F, make-only by default); Shift released first.
    press(k, 0xE5, true);   // Right Shift
    press(k, 0x45, true);
    CHECK(drain(k) == Bytes{0x59, 0xF0, 0x59, 0x5F});
    press(k, 0xE5, false);  // invisible: the terminal already saw it released
    press(k, 0x45, false);
    CHECK(drain(k).empty());

    // Unshifted F1 stays F1.
    press(k, 0x3A, true);
    press(k, 0x3A, false);
    CHECK(drain(k) == Bytes{0x07});
}

TEST_CASE("Shift+F13 mapping can be disabled and does not apply to Enhanced keyboards") {
    for (int variant = 0; variant < 2; ++variant) {
        const bool enhanced = variant == 1;
        Set3Keyboard k(enhanced ? KeyboardType::Enhanced : KeyboardType::Terminal122, 0xAB, 0x83);
        if (!enhanced) k.set_shift_fkeys(false);
        k.power_on();
        drain(k);
        press(k, 0xE1, true);
        press(k, 0x3A, true);
        press(k, 0x3A, false);
        press(k, 0xE1, false);
        CHECK(drain(k) == Bytes{0x12, 0x07, 0xF0, 0x12});
    }
}

TEST_CASE("Shifted upper-row key repeats as itself") {
    Set3Keyboard k(KeyboardType::Terminal122, 0xBF, 0xBF);
    k.power_on();
    drain(k);
    k.host_write(0xFB);   // make F13 typematic
    k.host_write(0x08);
    k.host_write(0xF4);
    drain(k);
    press(k, 0xE1, true);
    press(k, 0x3A, true);
    CHECK(drain(k) == Bytes{0x12, 0xF0, 0x12, 0x08});
    k.advance_us(600000);   // past the 500 ms delay
    const Bytes rep = drain(k);
    REQUIRE_FALSE(rep.empty());
    for (uint8_t b : rep) CHECK(b == 0x08);
    press(k, 0x3A, false);
    CHECK(drain(k) == Bytes{0x12});
}

TEST_CASE("cursor keys per key mode: make-only, make/break, typematic") {
    // The 3477 firmware programs the 122-key keyboard with F6, FD (make
    // only) 5A 58 11 7C, FB (typematic) <editing and keypad keys> and
    // FA 61 6A (horizontal cursor keys typematic; FA = FB on this keyboard).
    Set3Keyboard k;
    k.set_bat_sends_id(false);
    k.power_on();
    k.host_write(0xF9);   // everything make-only
    k.host_write(0xFA);
    k.host_write(0x61);
    k.host_write(0x6A);
    k.host_write(0xFC);   // up arrow make/break
    k.host_write(0x63);
    k.host_write(0xF3);
    k.host_write(0x20);   // 30 cps (33.36 ms), 500 ms delay
    k.host_write(0xF4);
    drain(k);
    constexpr uint32_t kRight = 0x4F, kDown = 0x51, kUp = 0x52;

    // Make-only: one make, no repeat, no break however long it is held.
    press(k, kDown, true);
    k.advance_us(2000000);
    press(k, kDown, false);
    CHECK(drain(k) == Bytes{0x60});

    // Make/break: make and break, no repeat.
    press(k, kUp, true);
    k.advance_us(2000000);
    press(k, kUp, false);
    CHECK(drain(k) == Bytes{0x63, 0xF0, 0x63});

    // Typematic (no break): a short tap is one make...
    press(k, kRight, true);
    k.advance_us(150000);
    press(k, kRight, false);
    CHECK(drain(k) == Bytes{0x6A});
    // ...a hold repeats after the delay, at the rate, and stops at release.
    press(k, kRight, true);
    k.advance_us(499000);
    CHECK(drain(k) == Bytes{0x6A});
    k.advance_us(1000 + 2 * 33360);
    CHECK(drain(k) == Bytes{0x6A, 0x6A, 0x6A});
    press(k, kRight, false);
    k.advance_us(1000000);
    CHECK_FALSE(k.has_output());
}

TEST_CASE("typematic runs in emulated time (3477 keyboard interface)") {
    // Repeats follow the machine's cycles, not the wall clock: a machine
    // running slower or faster than real time keeps the terminal's timing.
    const uint64_t hz = 7477312;
    Ibm3477KeyboardInterface kif(hz, KeyboardType::Terminal122, 0xBF, 0xBF);
    kif.reset();
    Set3Keyboard& k = kif.keyboard();
    drain(k);
    k.host_write(0xF3);
    k.host_write(0x00);   // 30 cps, 250 ms delay
    drain(k);
    press(k, 0x04, true);   // a: typematic by default
    CHECK(drain(k) == Bytes{0x1C});
    // 249 ms of machine time in small steps (the keyboard line stays held,
    // so nothing is delivered to the interface's receive register).
    for (int i = 0; i < 249; ++i) kif.tick(hz / 1000);
    CHECK_FALSE(k.has_output());
    for (int i = 0; i < 2; ++i) kif.tick(hz / 1000);
    CHECK(k.output_size() == 1);
}

TEST_CASE("terminal function keys bypass the host keymap (toolbar Attn, SysRq, Reset)") {
    Set3Keyboard k(KeyboardType::Enhanced, 0x7F, 0x7F);
    k.power_on();
    drain(k);
    REQUIRE(k.terminal_key("attn", true));
    REQUIRE(k.terminal_key("attn", false));
    CHECK(drain(k) == Bytes{0x08});              // Enhanced Attn: Esc position, make-only
    // The host's Shift is held: Attn releases the terminal Shift around it.
    press(k, 0xE1, true);
    CHECK(drain(k) == Bytes{0x12});
    REQUIRE(k.terminal_key("attn", true));
    REQUIRE(k.terminal_key("attn", false));
    CHECK(drain(k) == Bytes{0xF0, 0x12, 0x08, 0x12});
    press(k, 0xE1, false);
    drain(k);
    // SysRq on the Enhanced keyboard is Alt + Print Screen.
    REQUIRE(k.terminal_key("sysrq", true));
    REQUIRE(k.terminal_key("sysrq", false));
    const Bytes sysrq = drain(k);
    REQUIRE(sysrq.size() >= 2);
    CHECK(sysrq[0] == 0x19);                     // Left Alt make first
    CHECK(sysrq[1] == 0x57);
    CHECK(k.terminal_key("reset", true));
    CHECK(k.terminal_key("reset", false));
    CHECK_FALSE(k.terminal_key("no such key", true));
}

TEST_CASE("Esc and the toolbar Attn share set-3 05 without blocking each other (122-key)") {
    // Both GUI paths end at the same terminal key; a reference left behind by
    // one would silently swallow the other.
    Set3Keyboard k(KeyboardType::Terminal122, 0xBF, 0xBF);
    k.power_on();
    drain(k);
    std::string error;
    REQUIRE(k.set_host_keymap("pcomm", false, error));
    for (int round = 0; round < 2; ++round) {
        press(k, 0x29, true);   // Esc
        press(k, 0x29, false);
        CHECK(drain(k) == Bytes{0x05});
        REQUIRE(k.terminal_key("attn", true));
        REQUIRE(k.terminal_key("attn", false));
        CHECK(drain(k) == Bytes{0x05});
    }
    // Toolbar Attn while the host Esc is still down: the key is already made.
    press(k, 0x29, true);
    CHECK(drain(k) == Bytes{0x05});
    REQUIRE(k.terminal_key("attn", true));
    REQUIRE(k.terminal_key("attn", false));
    press(k, 0x29, false);
    drain(k);
    REQUIRE(k.terminal_key("attn", true));
    REQUIRE(k.terminal_key("attn", false));
    CHECK(drain(k) == Bytes{0x05});
}

TEST_CASE("host keymap replaced at run time") {
    Set3Keyboard k(KeyboardType::Enhanced, 0x7F, 0x7F);
    k.power_on();
    drain(k);
    std::string err;
    REQUIRE(k.set_host_keymap("pcomm", true, err));
    press(k, 0x28, true);   // Enter -> Field Exit (5A) under pcomm
    press(k, 0x28, false);
    const Bytes pcomm = drain(k);
    REQUIRE_FALSE(pcomm.empty());
    CHECK(pcomm[0] == 0x5A);
    CHECK_FALSE(k.set_host_keymap("/no/such/keymap.toml", true, err));
    CHECK_FALSE(err.empty());
}
