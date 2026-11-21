// SPDX-License-Identifier: GPL-3.0-or-later
// Device-side model of an IBM terminal keyboard (scan-code set 3), as the
// terminal's keyboard interface sees it.
//
// The model is byte-level: the interface hardware (not yet identified for any
// 34xx model) feeds host-to-keyboard bytes to host_write() and drains
// keyboard-to-host bytes with has_output()/read_output(). Framing, parity,
// clock/data line timing and inhibit are the interface's business.
//
// Host keys arrive as HostKeyEvent (USB HID usage codes). A host keymap
// (input/host_keymap.hpp) turns them into presses of terminal key positions;
// keys without a rule go to the set-3 code of the key at the same physical
// position on a 122-key or IBM Enhanced keyboard. See docs/keyboard.md.
#pragma once

#include "input/host_keymap.hpp"
#include "input/keyboard_ids.hpp"
#include "machine/peripherals.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>

namespace tn34xx {

class Set3Keyboard : public KeyboardSink {
public:
    // Per-key behaviour in scan-code set 3.
    enum KeyMode : uint8_t {
        MakeOnly = 0,
        Typematic = 1,           // make, repeated while held; no break
        MakeBreak = 2,           // make and break, no repeat
        TypematicMakeBreak = 3,  // all of them
    };

    // Default typematic setting (10.9 characters/s, 500 ms delay).
    static constexpr uint8_t kDefaultTypematic = 0x2B;
    static constexpr size_t kBufferLimit = 16;   // key bytes held before overrun

    Set3Keyboard(KeyboardType type = KeyboardType::Terminal122, uint8_t id1 = 0xBF,
                 uint8_t id2 = 0xBF);

    KeyboardType type() const { return type_; }
    void set_type(KeyboardType type) { type_ = type; }
    void set_id(uint8_t id1, uint8_t id2) { id1_ = id1; id2_ = id2; }
    uint8_t id1() const { return id1_; }
    uint8_t id2() const { return id2_; }

    // Send the two ID bytes after AA on power-on/reset (observed on the
    // 122-key 1390876; default on for Terminal122 only).
    void set_bat_sends_id(bool on) { bat_sends_id_ = on; }
    bool bat_sends_id() const { return bat_sends_id_; }

    // Power-on reset: clears state and queues the BAT result.
    void power_on();

    // Host-to-keyboard byte (command or command parameter).
    void host_write(uint8_t byte);

    bool has_output() const { return !out_.empty(); }
    size_t output_size() const { return out_.size(); }
    uint8_t read_output();

    // Host key from the front end.
    void host_key(const HostKeyEvent& ev) override;

    bool terminal_key(const std::string& function, bool pressed) override;
    bool set_host_keymap(const std::string& spec, bool shift_fkeys, std::string& error) override;

    // Host keymap applied to host_key(). The default is the built-in
    // "positional" map.
    void set_keymap(std::shared_ptr<const Keymap> km) { keymap_.set_keymap(std::move(km)); }
    const Keymap* keymap() const { return keymap_.keymap(); }

    // Host convention for keys a PC lacks: Shift+F1..F12 enters F13..F24 on
    // a 122-key keyboard (on by default). The terminal sees the upper-row key
    // unshifted: the held Shift keys are released (break codes) before the
    // F13+ make and pressed again (make codes) after its break. Selects the
    // built-in "positional" keymap with or without these rules.
    void set_shift_fkeys(bool on);
    bool shift_fkeys() const { return shift_fkeys_; }
    // A key at set-3 position `code` changed state.
    void key(uint8_t code, bool pressed);

    // Advance keyboard time, generating typematic repeats.
    void advance_us(uint64_t us);

    // HID usage -> set-3 make code for a keyboard type; 0 if unmapped.
    static uint8_t hid_to_set3(KeyboardType type, uint32_t hid_usage);
    // Power-on default mode of a set-3 key.
    static KeyMode default_key_mode(uint8_t code);

    KeyMode key_mode(uint8_t code) const { return KeyMode(modes_[code]); }
    uint8_t leds() const { return leds_; }  // bit 0 Scroll, 1 Num, 2 Caps
    bool scroll_lock_led() const { return leds_ & 1; }
    bool num_lock_led() const { return leds_ & 2; }
    bool caps_lock_led() const { return leds_ & 4; }
    bool scanning() const { return scanning_; }
    uint8_t typematic() const { return typematic_; }
    uint8_t requested_set() const { return set_; }

private:
    enum class Pending { None, Leds, ScanSet, Typematic, KeyList };

    void command(uint8_t cmd);
    void reset_defaults();
    void respond(uint8_t b);
    void clear_key_bytes();   // F4/F5/F6 empty the key buffer
    void key_byte(uint8_t b);
    void set_all(KeyMode m);
    uint64_t typematic_delay_us() const;
    uint64_t typematic_period_us() const;

    KeyboardType type_;
    uint8_t id1_, id2_;
    bool bat_sends_id_;

    std::deque<uint16_t> out_;  // bit 8 marks a key (not response) byte
    size_t key_bytes_ = 0;      // key bytes in out_, for overrun detection
    bool overrun_ = false;
    uint8_t last_sent_ = 0xAA;

    Pending pending_ = Pending::None;
    KeyMode list_mode_ = MakeOnly;
    std::array<uint8_t, 256> modes_{};
    std::array<bool, 256> held_{};
    uint8_t leds_ = 0;
    uint8_t typematic_ = kDefaultTypematic;
    uint8_t set_ = 3;
    bool scanning_ = true;

    int repeat_code_ = -1;      // key currently repeating, -1 none

    bool shift_fkeys_ = true;
    HostKeymapEngine keymap_{&Set3Keyboard::hid_to_set3};
    uint64_t repeat_wait_us_ = 0;
};

} // namespace tn34xx
