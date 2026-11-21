// SPDX-License-Identifier: GPL-3.0-or-later
// End to end, 122-key keyboard: host key -> host keymap -> set-3 keyboard
// bytes -> 5250 scan codes on the cable -> controller operator action.
//
// The terminal's own step (set-3 key position -> cable code) is firmware, so
// it is stood in for here by the codes the 3477 was observed to send, which
// are those of PAI fig. 105 (docs/keyboard.md, "5250 scan codes on the
// cable"). The table is written position -> code, the inverse of the
// controller's decoder, so the two are checked against each other.
#include "host/wsc/keyboard_map.hpp"
#include "input/host_keymap.hpp"
#include "input/set3_keyboard.hpp"

#include <doctest/doctest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace em34xx;
using namespace em34xx::wsc;

namespace {

using AK = OperatorAction::Kind;

// Observed: set-3 key position -> cable code for the keys without a shift
// requirement (3477, keyboard 122/us).
const std::map<uint8_t, uint8_t> kObserved = {
    {0x05, 0x7C}, {0x04, 0x66}, {0x83, 0x78}, {0x06, 0x75}, {0x0C, 0x67}, {0x0B, 0x7D},
    {0x0E, 0x3E}, {0x16, 0x31}, {0x1E, 0x32}, {0x26, 0x33}, {0x25, 0x34}, {0x2E, 0x35}, {0x36, 0x36},
    {0x3D, 0x37}, {0x3E, 0x38}, {0x46, 0x39}, {0x45, 0x3A}, {0x4E, 0x3B}, {0x55, 0x3C}, {0x66, 0x3D},
    {0x0D, 0x20}, {0x15, 0x21}, {0x1D, 0x22}, {0x24, 0x23}, {0x2D, 0x24}, {0x2C, 0x25}, {0x35, 0x26},
    {0x3C, 0x27}, {0x43, 0x28}, {0x44, 0x29}, {0x4D, 0x2A}, {0x54, 0x2B}, {0x5B, 0x2C}, {0x5A, 0x2D},
    {0x1C, 0x11}, {0x1B, 0x12}, {0x23, 0x13}, {0x2B, 0x14}, {0x34, 0x15}, {0x33, 0x16}, {0x3B, 0x17},
    {0x42, 0x18}, {0x4B, 0x19}, {0x4C, 0x1A}, {0x52, 0x1B}, {0x53, 0x1C},
    {0x13, 0x0E}, {0x1A, 0x01}, {0x22, 0x02}, {0x21, 0x03}, {0x2A, 0x04}, {0x32, 0x05}, {0x31, 0x06},
    {0x3A, 0x07}, {0x41, 0x08}, {0x49, 0x09}, {0x4A, 0x0A},
    {0x11, 0x7E}, {0x29, 0x0F}, {0x58, 0x68},
    {0x67, 0x4B}, {0x6E, 0x4C}, {0x6F, 0x62}, {0x64, 0x0C},
    {0x63, 0x71}, {0x61, 0x72}, {0x6A, 0x73}, {0x60, 0x70},
    {0x76, 0x1D}, {0x77, 0x1E}, {0x7E, 0x4F}, {0x84, 0x50},
    {0x6C, 0x47}, {0x75, 0x48}, {0x7D, 0x49}, {0x7C, 0x4E},
    {0x6B, 0x44}, {0x73, 0x45}, {0x74, 0x46}, {0x7B, 0x55},
    {0x69, 0x41}, {0x72, 0x42}, {0x7A, 0x43}, {0x79, 0x4D},
    {0x70, 0x40}, {0x71, 0x4A},
};

const uint8_t kLowerF[12] = {0x07, 0x0F, 0x17, 0x1F, 0x27, 0x2F, 0x37, 0x3F, 0x47, 0x4F, 0x56, 0x5E};
const uint8_t kUpperF[12] = {0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x57, 0x5F};

// Stand-in for the terminal: set-3 bytes in, cable codes out. Shift is sent
// as make/break 57h/56h, Alt as the CMD prefix 6Fh, and keys that need a
// given shift state get the terminal's own shift make/break around them
// (observed; PAI fig. 105 notes 1-4).
struct Terminal {
    bool lshift = false, rshift = false, alt = false;
    bool brk = false;
    std::vector<uint8_t> cable;

    bool shifted() const { return lshift || rshift; }

    void wrapped(uint8_t code, bool need_upper, bool cmd) {
        const bool change = need_upper != shifted();
        if (cmd) cable.push_back(0x6F);
        if (change) cable.push_back(need_upper ? 0x57 : 0xD7);
        cable.push_back(code);
        if (change) cable.push_back(need_upper ? 0xD7 : 0x57);
    }

    void byte(uint8_t b) {
        if (b == 0xF0) {
            brk = true;
            return;
        }
        const bool make = !brk;
        brk = false;
        switch (b) {
        case 0x12: lshift = make; cable.push_back(make ? 0x57 : 0xD7); return;
        case 0x59: rshift = make; cable.push_back(make ? 0x56 : 0xD6); return;
        case 0x14: cable.push_back(make ? 0x54 : 0xD4); return;
        case 0x19: case 0x39: alt = make; return;
        default: break;
        }
        if (!make) return;
        for (int i = 0; i < 12; ++i) {
            if (b == kLowerF[i]) return wrapped(uint8_t(0x31 + i), false, true);
            if (b == kUpperF[i]) return wrapped(uint8_t(0x31 + i), true, true);
        }
        if (b == 0x65) return wrapped(0x6C, false, alt);   // Insert
        if (b == 0x6D) return wrapped(0x6C, true, alt);    // Delete
        const auto it = kObserved.find(b);
        REQUIRE(it != kObserved.end());
        if (alt) cable.push_back(0x6F);
        cable.push_back(it->second);
    }
};

// The keyboard modes the 3477 firmware programs (observed): FD 5A 58 11 7C,
// FB for the editing and keypad keys, FA 61 6A (= FB on this keyboard).
void program_like_3477(Set3Keyboard& k) {
    const uint8_t seq[] = {0xF6, 0xFD, 0x5A, 0x58, 0x11, 0x7C, 0xFB, 0x67, 0x69, 0x72, 0x7A, 0x6B, 0x73,
                           0x74, 0x6C, 0x75, 0x7D, 0x70, 0x76, 0x77, 0x7E, 0x84, 0x6E, 0x71, 0xFA, 0x61,
                           0x6A, 0xF4};
    for (uint8_t b : seq) k.host_write(b);
    while (k.has_output()) k.read_output();
}

struct Chain {
    Set3Keyboard kbd{KeyboardType::Terminal122, 0xBF, 0xBF};
    Terminal term;
    KeyboardMap km{0x02};

    Chain() {
        auto map = std::make_shared<Keymap>();
        REQUIRE(builtin_keymap("pcomm", true, *map));
        kbd.set_keymap(map);
        kbd.set_bat_sends_id(false);
        kbd.power_on();
        while (kbd.has_output()) kbd.read_output();
        program_like_3477(kbd);
    }

    uint32_t hid(const char* name) {
        uint32_t h = 0;
        REQUIRE(host_key_from_name(name, h));
        return h;
    }

    // Presses the host keys in order and releases them in reverse; returns
    // the controller actions.
    std::vector<OperatorAction> chord(std::vector<const char*> keys) {
        for (const char* k : keys) kbd.host_key(HostKeyEvent{hid(k), true, k});
        for (auto it = keys.rbegin(); it != keys.rend(); ++it) kbd.host_key(HostKeyEvent{hid(*it), false, *it});
        while (kbd.has_output()) term.byte(kbd.read_output());
        std::vector<OperatorAction> out;
        for (uint8_t c : term.cable) {
            OperatorAction a;
            if (km.translate(c, a)) out.push_back(a);
        }
        term.cable.clear();
        return out;
    }

    OperatorAction one(std::vector<const char*> keys) {
        const std::vector<OperatorAction> v = chord(std::move(keys));
        REQUIRE(v.size() == 1);
        return v[0];
    }
};

} // namespace

TEST_CASE("122-key end to end: letters reach the cable as fig. 105 codes") {
    Chain c;
    c.kbd.host_key(HostKeyEvent{c.hid("a"), true, "a"});
    c.kbd.host_key(HostKeyEvent{c.hid("a"), false, "a"});
    std::vector<uint8_t> set3;
    while (c.kbd.has_output()) set3.push_back(c.kbd.read_output());
    CHECK(set3 == std::vector<uint8_t>{0x1C});   // typematic: make only
    for (uint8_t b : set3) c.term.byte(b);
    CHECK(c.term.cable == std::vector<uint8_t>{0x11});   // observed for 'a'
    c.term.cable.clear();

    const char* const keys[] = {"a", "b", "c"};
    const uint8_t ebcdic[] = {0x81, 0x82, 0x83};
    for (int i = 0; i < 3; ++i) {
        const OperatorAction a = c.one({keys[i]});
        CHECK(a.kind == AK::Character);
        CHECK(a.ebcdic == ebcdic[i]);
    }
}

TEST_CASE("122-key end to end: PCOMM map, host key to operator action") {
    Chain c;
    OperatorAction a = c.one({"LShift", "a"});
    CHECK(a.ebcdic == 0xC1);
    a = c.one({"1"});
    CHECK(a.ebcdic == 0xF1);
    a = c.one({"RShift", "1"});
    CHECK(a.ebcdic == 0x5A);
    a = c.one({"0"});
    CHECK(a.ebcdic == 0xF0);
    a = c.one({"space"});
    CHECK(a.ebcdic == 0x40);

    // F1..F12 and Shift+F1..F12 (= F13..F24 on the upper row).
    const char* const fk[12] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    for (int i = 0; i < 12; ++i) {
        CAPTURE(i);
        a = c.one({fk[i]});
        CHECK(a.kind == AK::Aid);
        CHECK(a.aid == 0x31 + i);
        a = c.one({"LShift", fk[i]});
        CHECK(a.kind == AK::Aid);
        CHECK(a.aid == 0xB1 + i);
    }
    CHECK_FALSE(c.km.shift());

    a = c.one({"RCtrl"});   // Enter
    CHECK(a.kind == AK::Aid);
    CHECK(a.aid == 0xF1);
    a = c.one({"Return"});   // Field Exit
    CHECK(a.function == KeyFunction::FieldExit);
    a = c.one({"LShift", "Return"});   // New Line
    CHECK(a.function == KeyFunction::NewLine);
    a = c.one({"LCtrl"});   // Reset
    CHECK(a.function == KeyFunction::Reset);
    a = c.one({"Escape"});   // Attn
    CHECK(a.kind == AK::Attn);
    a = c.one({"LShift", "Escape"});   // Sys Req
    CHECK(a.kind == AK::SysReq);
    a = c.one({"Tab"});
    CHECK(a.function == KeyFunction::Tab);
    a = c.one({"LShift", "Tab"});
    CHECK(a.function == KeyFunction::Backtab);
    a = c.one({"Insert"});
    CHECK(a.function == KeyFunction::Insert);
    a = c.one({"Delete"});
    CHECK(a.function == KeyFunction::Delete);
    a = c.one({"Left"});
    CHECK(a.function == KeyFunction::CursorLeft);
    a = c.one({"PageUp"});   // Roll Down
    CHECK(a.kind == AK::Aid);
    CHECK(a.aid == 0xF4);
    a = c.one({"PageDown"});   // Roll Up
    CHECK(a.aid == 0xF5);
    a = c.one({"LAlt", "F1"});   // Help
    CHECK(a.kind == AK::Aid);
    CHECK(a.aid == 0xF3);
    a = c.one({"KP_Plus"});
    CHECK(a.function == KeyFunction::FieldPlus);
    a = c.one({"KP_Minus"});
    CHECK(a.function == KeyFunction::FieldMinus);
    CHECK_FALSE(c.km.shift());
    CHECK_FALSE(c.km.cmd_pending());
}

TEST_CASE("122-key end to end: Esc is Attn, Shift+Esc Sys Req, also under Caps Lock") {
    Chain c;
    OperatorAction a = c.one({"Escape"});
    CHECK(a.kind == AK::Attn);
    a = c.one({"LShift", "Escape"});
    CHECK(a.kind == AK::SysReq);
    a = c.one({"Escape"});   // Shift released: Attn again
    CHECK(a.kind == AK::Attn);
    a = c.one({"RShift", "Escape"});
    CHECK(a.kind == AK::SysReq);

    // Caps Lock is the 122-key Shift Lock (54h). It shifts the typing keys
    // only; Esc stays Attn, F1 stays F1, Up stays cursor up.
    CHECK(c.chord({"CapsLock"}).empty());
    CHECK(c.km.caps_lock());
    a = c.one({"Escape"});
    CHECK(a.kind == AK::Attn);
    a = c.one({"F1"});
    CHECK(a.kind == AK::Aid);
    CHECK(a.aid == 0x31);
    a = c.one({"Up"});
    CHECK(a.function == KeyFunction::CursorUp);
    a = c.one({"Tab"});
    CHECK(a.function == KeyFunction::Tab);
    a = c.one({"a"});
    CHECK(a.ebcdic == 0xC1);
    a = c.one({"1"});
    CHECK(a.ebcdic == 0x5A);
    CHECK(c.chord({"CapsLock"}).empty());   // pressing it again keeps the lock
    a = c.one({"Escape"});
    CHECK(a.kind == AK::Attn);
    a = c.one({"LShift", "Escape"});   // a Shift key releases the lock
    CHECK(a.kind == AK::SysReq);
    CHECK_FALSE(c.km.caps_lock());
    a = c.one({"Escape"});
    CHECK(a.kind == AK::Attn);
}
