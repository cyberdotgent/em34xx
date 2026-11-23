// SPDX-License-Identifier: GPL-3.0-or-later
// Host key codes for the Qt front end: QKeyEvent::nativeScanCode() to USB
// HID usages (Keyboard/Keypad page 0x07), the form Machine::key_event takes.
#pragma once

#include <cstdint>

namespace em34xx::qt {

// `xkb_keycode`: QKeyEvent::nativeScanCode() on Qt's xcb and wayland
// platforms, an XKB keycode. With the evdev keycode set (every current
// Linux X server and Wayland compositor; documented by xkeyboard-config)
// that is the kernel's input event code plus 8. Returns 0 for a key with no
// usage on the keyboard page (media keys and the like).
uint32_t hid_from_xkb_keycode(uint32_t xkb_keycode);

// A short name for the log ("A", "KP_ENTER", "F13"); "" when unknown.
const char* hid_key_name(uint32_t hid_usage);

} // namespace em34xx::qt
