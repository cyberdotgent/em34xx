// Host keymap: PC keys (USB HID usages, as SDL scancodes) plus host modifier
// state -> presses of terminal key positions (scan-code set 3), applied before
// the Set3Keyboard model.
//
// The terminal keyboard is a real IBM 122-key or Enhanced keyboard; the
// terminal firmware gives each key position its meaning. A keymap therefore
// says which terminal key (and whether the terminal's own Shift/Alt is held
// or released around it) a host key combination presses. Keys without a rule
// fall back to the key at the same physical position (Set3Keyboard's table).
//
// Two built-in maps exist: "positional" (physical position only, plus
// Shift+F1..F12 -> F13..F24 on the 122-key keyboard) and "pcomm" (the
// assignments of IBM Personal Communications' 5250 default keyboard, expressed
// in terminal key positions). TOML files extend either. See docs/keyboard.md.
#pragma once

#include "input/keyboard_ids.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tn34xx {

// What happens to one of the terminal's modifier groups (both Shift keys, or
// both Alt keys) while a mapped key is down.
enum class ModAction : uint8_t {
    Keep,      // whatever the host's own Shift/Alt keys are doing
    Press,     // held (Left Shift / Left Alt pressed if none is)
    Release,   // released (break codes for the held keys)
};

// One terminal key press: a set-3 key position and the terminal modifiers
// around it. code 0 = no terminal key (the host key is swallowed).
struct TerminalStroke {
    uint8_t code = 0;
    ModAction shift = ModAction::Keep;
    ModAction alt = ModAction::Keep;
    bool operator==(const TerminalStroke& o) const {
        return code == o.code && shift == o.shift && alt == o.alt;
    }
    bool operator!=(const TerminalStroke& o) const { return !(*this == o); }
};

// Condition on one host modifier group (Shift, Ctrl or Alt).
enum class ModCond : uint8_t {
    Off,      // neither key of the group held
    Either,   // at least one held
    Left,     // the left key held (the right one is not looked at)
    Right,    // the right key held (the left one is not looked at)
    Ignore,   // not looked at
};

struct KeymapRule {
    uint32_t hid = 0;
    ModCond shift = ModCond::Off;
    ModCond ctrl = ModCond::Off;
    ModCond alt = ModCond::Off;
    // Action per keyboard type (index 0 = 122-key, 1 = Enhanced); a rule
    // only applies to the types where `applies` is set.
    std::array<bool, 2> applies{{true, true}};
    std::array<TerminalStroke, 2> to{};
    // The key behaves as the physical-position key (pass-through), not `to`.
    bool positional = false;
    // Dual-role key: the stroke is sent when the key is released, and only
    // if no other key was pressed meanwhile (PCOMM's Enter/Ctrl and
    // Reset/Ctrl). While held the key acts as a host modifier only.
    bool tap = false;
    std::string source;   // "pcomm", "positional" or file:line, for messages

    int specificity() const;
};

class Keymap {
public:
    void add(const KeymapRule& r) { rules_.push_back(r); }
    const std::vector<KeymapRule>& rules() const { return rules_; }

    // Best rule for a key and host modifier state (HostMod bits), or null.
    // The most specific matching rule wins; among equals, the last added.
    const KeymapRule* find(KeyboardType type, uint32_t hid, uint8_t mods) const;

private:
    std::vector<KeymapRule> rules_;
};

// Host modifier bits.
enum HostMod : uint8_t {
    kModLShift = 0x01, kModRShift = 0x02,
    kModLCtrl = 0x04, kModRCtrl = 0x08,
    kModLAlt = 0x10, kModRAlt = 0x20,
};
uint8_t host_modifier_bit(uint32_t hid);   // 0 if not a Shift/Ctrl/Alt key

// Built-in keymaps: "positional" and "pcomm". shift_fkeys adds the 122-key
// Shift+F1..F12 -> F13..F24 rules (both maps). False if the name is unknown.
bool builtin_keymap(const std::string& name, bool shift_fkeys, Keymap& out);

// `spec` is a built-in name or a TOML file (see docs/keyboard.md).
bool load_keymap(const std::string& spec, bool shift_fkeys, Keymap& out, std::string& error);
// Parse keymap TOML text; `origin` names it in error messages.
bool parse_keymap(const std::string& text, const std::string& origin, bool shift_fkeys, Keymap& out,
                  std::string& error);

// Name tables used by keymap files (names are case-insensitive; '-', '_'
// and spaces are ignored).
bool host_key_from_name(const std::string& name, uint32_t& hid);
// A terminal function (by its legend on the IBM 122-key / Enhanced keyboards)
// or a raw set-3 position ("0x5A"). ok[i] is false where a keyboard type has
// no such key.
bool terminal_target_from_name(const std::string& name, std::array<TerminalStroke, 2>& to,
                               std::array<bool, 2>& ok);
// Every terminal function name, for documentation and tests.
std::vector<std::string> terminal_function_names();

// Applies a keymap to host key events and keeps the terminal-side state:
// which terminal keys each host key pressed (so that the release matches the
// press whatever the modifiers do meanwhile), and the terminal's Shift/Alt
// keys as the host holds them, overridden by the keys currently down.
class HostKeymapEngine {
public:
    using Emit = std::function<void(uint8_t code, bool pressed)>;
    using Positional = uint8_t (*)(KeyboardType, uint32_t);

    HostKeymapEngine(Positional positional) : positional_(positional) {}

    void set_keymap(std::shared_ptr<const Keymap> km) { keymap_ = std::move(km); }
    const Keymap* keymap() const { return keymap_.get(); }

    // False if the key maps to nothing (for logging by the caller).
    bool host_key(KeyboardType type, uint32_t hid, bool pressed, const Emit& emit);
    // Press or release one terminal stroke directly (no host key, no
    // keymap): a toolbar's Attn button. Calls must pair up.
    void stroke(const TerminalStroke& s, bool pressed, const Emit& emit) {
        if (pressed) press_stroke(s, false, emit);
        else release_stroke(s, false, emit);
    }
    // Forget all held keys without emitting anything (keyboard reset).
    void reset();

    uint8_t host_mods() const { return host_mods_; }

private:
    struct Down {
        TerminalStroke stroke;   // what was pressed (applied when not tap)
        bool pass_mod = false;   // stroke.code is a terminal modifier held as-is
        bool tap = false;
        bool consumed = false;
    };

    void press_stroke(const TerminalStroke& s, bool pass_mod, const Emit& emit);
    void release_stroke(const TerminalStroke& s, bool pass_mod, const Emit& emit);
    void sync_mods(const Emit& emit);

    Positional positional_;
    std::shared_ptr<const Keymap> keymap_;
    std::map<uint32_t, Down> down_;
    uint8_t host_mods_ = 0;
    std::array<uint8_t, 256> refs_{};   // host keys holding each terminal key
    // Terminal modifier keys: Left Shift 12, Right Shift 59, Left Alt 19,
    // Right Alt 39.
    std::array<uint8_t, 4> phys_{};     // held as-is by host keys (count)
    std::array<bool, 4> sent_{};        // state the terminal has seen
    int shift_off_ = 0, shift_on_ = 0, alt_off_ = 0, alt_on_ = 0;
};

} // namespace tn34xx
