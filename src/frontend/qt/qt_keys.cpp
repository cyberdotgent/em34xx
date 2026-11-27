// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/qt/qt_keys.hpp"

namespace em34xx::qt {

namespace {

struct Key {
    uint16_t evdev;     // Linux input event code (KEY_*, input-event-codes.h)
    uint16_t hid;       // USB HID usage, Keyboard/Keypad page
    const char* name;
};

// Positions, not characters: the row a key sits in on a US keyboard names
// it. Compiled from the two public numbering schemes (the kernel's KEY_*
// codes and the HID Usage Tables, section 10).
constexpr Key kKeys[] = {
    {1, 0x29, "ESC"},
    {2, 0x1E, "1"}, {3, 0x1F, "2"}, {4, 0x20, "3"}, {5, 0x21, "4"}, {6, 0x22, "5"},
    {7, 0x23, "6"}, {8, 0x24, "7"}, {9, 0x25, "8"}, {10, 0x26, "9"}, {11, 0x27, "0"},
    {12, 0x2D, "MINUS"}, {13, 0x2E, "EQUAL"}, {14, 0x2A, "BACKSPACE"}, {15, 0x2B, "TAB"},
    {16, 0x14, "Q"}, {17, 0x1A, "W"}, {18, 0x08, "E"}, {19, 0x15, "R"}, {20, 0x17, "T"},
    {21, 0x1C, "Y"}, {22, 0x18, "U"}, {23, 0x0C, "I"}, {24, 0x12, "O"}, {25, 0x13, "P"},
    {26, 0x2F, "LEFTBRACE"}, {27, 0x30, "RIGHTBRACE"}, {28, 0x28, "ENTER"}, {29, 0xE0, "LEFTCTRL"},
    {30, 0x04, "A"}, {31, 0x16, "S"}, {32, 0x07, "D"}, {33, 0x09, "F"}, {34, 0x0A, "G"},
    {35, 0x0B, "H"}, {36, 0x0D, "J"}, {37, 0x0E, "K"}, {38, 0x0F, "L"},
    {39, 0x33, "SEMICOLON"}, {40, 0x34, "APOSTROPHE"}, {41, 0x35, "GRAVE"}, {42, 0xE1, "LEFTSHIFT"},
    // The kernel reports the ISO key left of Enter (HID 0x32) as KEY_BACKSLASH
    // too; the two never sit on one keyboard.
    {43, 0x31, "BACKSLASH"},
    {44, 0x1D, "Z"}, {45, 0x1B, "X"}, {46, 0x06, "C"}, {47, 0x19, "V"}, {48, 0x05, "B"},
    {49, 0x11, "N"}, {50, 0x10, "M"},
    {51, 0x36, "COMMA"}, {52, 0x37, "DOT"}, {53, 0x38, "SLASH"}, {54, 0xE5, "RIGHTSHIFT"},
    {55, 0x55, "KP_ASTERISK"}, {56, 0xE2, "LEFTALT"}, {57, 0x2C, "SPACE"}, {58, 0x39, "CAPSLOCK"},
    {59, 0x3A, "F1"}, {60, 0x3B, "F2"}, {61, 0x3C, "F3"}, {62, 0x3D, "F4"}, {63, 0x3E, "F5"},
    {64, 0x3F, "F6"}, {65, 0x40, "F7"}, {66, 0x41, "F8"}, {67, 0x42, "F9"}, {68, 0x43, "F10"},
    {69, 0x53, "NUMLOCK"}, {70, 0x47, "SCROLLLOCK"},
    {71, 0x5F, "KP_7"}, {72, 0x60, "KP_8"}, {73, 0x61, "KP_9"}, {74, 0x56, "KP_MINUS"},
    {75, 0x5C, "KP_4"}, {76, 0x5D, "KP_5"}, {77, 0x5E, "KP_6"}, {78, 0x57, "KP_PLUS"},
    {79, 0x59, "KP_1"}, {80, 0x5A, "KP_2"}, {81, 0x5B, "KP_3"}, {82, 0x62, "KP_0"}, {83, 0x63, "KP_DOT"},
    {85, 0x94, "ZENKAKUHANKAKU"}, {86, 0x64, "102ND"}, {87, 0x44, "F11"}, {88, 0x45, "F12"},
    {89, 0x87, "RO"}, {90, 0x92, "KATAKANA"}, {91, 0x93, "HIRAGANA"}, {92, 0x8A, "HENKAN"},
    {93, 0x88, "KATAKANAHIRAGANA"}, {94, 0x8B, "MUHENKAN"}, {95, 0x8C, "KP_JPCOMMA"},
    {96, 0x58, "KP_ENTER"}, {97, 0xE4, "RIGHTCTRL"}, {98, 0x54, "KP_SLASH"}, {99, 0x46, "SYSRQ"},
    {100, 0xE6, "RIGHTALT"},
    {102, 0x4A, "HOME"}, {103, 0x52, "UP"}, {104, 0x4B, "PAGEUP"}, {105, 0x50, "LEFT"},
    {106, 0x4F, "RIGHT"}, {107, 0x4D, "END"}, {108, 0x51, "DOWN"}, {109, 0x4E, "PAGEDOWN"},
    {110, 0x49, "INSERT"}, {111, 0x4C, "DELETE"},
    {117, 0x67, "KP_EQUAL"}, {119, 0x48, "PAUSE"}, {121, 0x85, "KP_COMMA"},
    {122, 0x90, "HANGEUL"}, {123, 0x91, "HANJA"}, {124, 0x89, "YEN"},
    {125, 0xE3, "LEFTMETA"}, {126, 0xE7, "RIGHTMETA"}, {127, 0x65, "COMPOSE"},
    {183, 0x68, "F13"}, {184, 0x69, "F14"}, {185, 0x6A, "F15"}, {186, 0x6B, "F16"},
    {187, 0x6C, "F17"}, {188, 0x6D, "F18"}, {189, 0x6E, "F19"}, {190, 0x6F, "F20"},
    {191, 0x70, "F21"}, {192, 0x71, "F22"}, {193, 0x72, "F23"}, {194, 0x73, "F24"},
};

constexpr uint32_t kXkbEvdevOffset = 8;

} // namespace

uint32_t hid_from_xkb_keycode(uint32_t xkb_keycode) {
    if (xkb_keycode < kXkbEvdevOffset) return 0;
    const uint32_t evdev = xkb_keycode - kXkbEvdevOffset;
    for (const Key& k : kKeys)
        if (k.evdev == evdev) return k.hid;
    return 0;
}

const char* hid_key_name(uint32_t hid_usage) {
    for (const Key& k : kKeys)
        if (k.hid == hid_usage) return k.name;
    return "";
}

} // namespace em34xx::qt
