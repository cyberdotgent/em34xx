// SPDX-License-Identifier: GPL-3.0-or-later
#include "input/set3_keyboard.hpp"

#include "util/log.hpp"

namespace em34xx {

namespace {

struct HidMap {
    uint8_t hid;
    uint8_t code;
};

// Keys whose set-3 code is the same on both keyboard types (main typing
// block, cursor/edit cluster, keypad digits).
const HidMap kCommon[] = {
    // letters a..z
    {0x04, 0x1C}, {0x05, 0x32}, {0x06, 0x21}, {0x07, 0x23}, {0x08, 0x24}, {0x09, 0x2B},
    {0x0A, 0x34}, {0x0B, 0x33}, {0x0C, 0x43}, {0x0D, 0x3B}, {0x0E, 0x42}, {0x0F, 0x4B},
    {0x10, 0x3A}, {0x11, 0x31}, {0x12, 0x44}, {0x13, 0x4D}, {0x14, 0x15}, {0x15, 0x2D},
    {0x16, 0x1B}, {0x17, 0x2C}, {0x18, 0x3C}, {0x19, 0x2A}, {0x1A, 0x1D}, {0x1B, 0x22},
    {0x1C, 0x35}, {0x1D, 0x1A},
    // digits 1..9, 0
    {0x1E, 0x16}, {0x1F, 0x1E}, {0x20, 0x26}, {0x21, 0x25}, {0x22, 0x2E}, {0x23, 0x36},
    {0x24, 0x3D}, {0x25, 0x3E}, {0x26, 0x46}, {0x27, 0x45},
    {0x28, 0x5A},  // Enter
    {0x2A, 0x66},  // Backspace
    {0x2B, 0x0D},  // Tab
    {0x2C, 0x29},  // Space
    {0x2D, 0x4E},  // -
    {0x2E, 0x55},  // =
    {0x2F, 0x54},  // [
    {0x30, 0x5B},  // ]
    {0x32, 0x53},  // non-US # (ISO key left of Enter)
    {0x33, 0x4C},  // ;
    {0x34, 0x52},  // '
    {0x35, 0x0E},  // `
    {0x36, 0x41},  // ,
    {0x37, 0x49},  // .
    {0x38, 0x4A},  // /
    {0x39, 0x14},  // Caps Lock
    // F1..F12 (the lower function-key row on a 122-key keyboard)
    {0x3A, 0x07}, {0x3B, 0x0F}, {0x3C, 0x17}, {0x3D, 0x1F}, {0x3E, 0x27}, {0x3F, 0x2F},
    {0x40, 0x37}, {0x41, 0x3F}, {0x42, 0x47}, {0x43, 0x4F}, {0x44, 0x56}, {0x45, 0x5E},
    // Insert Home PgUp / Delete End PgDn
    {0x49, 0x67}, {0x4A, 0x6E}, {0x4B, 0x6F}, {0x4C, 0x64}, {0x4D, 0x65}, {0x4E, 0x6D},
    // Right Left Down Up
    {0x4F, 0x6A}, {0x50, 0x61}, {0x51, 0x60}, {0x52, 0x63},
    {0x53, 0x76},  // Num Lock (top-left keypad key)
    {0x54, 0x77},  // keypad /
    {0x55, 0x7E},  // keypad *
    {0x56, 0x84},  // keypad - (top-right keypad key)
    {0x57, 0x7C},  // keypad +
    {0x58, 0x79},  // keypad Enter
    // keypad 1..9, 0, .
    {0x59, 0x69}, {0x5A, 0x72}, {0x5B, 0x7A}, {0x5C, 0x6B}, {0x5D, 0x73}, {0x5E, 0x74},
    {0x5F, 0x6C}, {0x60, 0x75}, {0x61, 0x7D}, {0x62, 0x70}, {0x63, 0x71},
    {0x64, 0x13},  // non-US \ (ISO key left of Z)
    {0x48, 0x62},  // Pause
    {0x87, 0x51},  // International1 (Ro)
    {0x88, 0x87},  // International2 (Katakana/Hiragana)
    {0x89, 0x5D},  // International3 (Yen)
    {0x8A, 0x86},  // International4 (Henkan)
    {0x8B, 0x85},  // International5 (Muhenkan)
    {0xE0, 0x11},  // Left Ctrl
    {0xE1, 0x12},  // Left Shift
    {0xE2, 0x19},  // Left Alt
    {0xE4, 0x58},  // Right Ctrl
    {0xE5, 0x59},  // Right Shift
    {0xE6, 0x39},  // Right Alt
};

// IBM Enhanced (101/102-key) terminal keyboard.
const HidMap kEnhanced[] = {
    {0x29, 0x08},  // Esc
    {0x31, 0x5C},  // backslash (above Enter on the 101-key layout)
    {0x46, 0x57},  // Print Screen
    {0x47, 0x5F},  // Scroll Lock
    {0xE3, 0x8B},  // Left GUI
    {0xE7, 0x8C},  // Right GUI
    {0x65, 0x8D},  // Application / Menu
};

// 122-key terminal keyboard. Keys a PC keyboard lacks are reached through the
// PC keys at roughly the same place (host-mapping choice, docs/keyboard.md).
const HidMap k122[] = {
    // F13..F24 (upper function-key row)
    {0x68, 0x08}, {0x69, 0x10}, {0x6A, 0x18}, {0x6B, 0x20}, {0x6C, 0x28}, {0x6D, 0x30},
    {0x6E, 0x38}, {0x6F, 0x40}, {0x70, 0x48}, {0x71, 0x50}, {0x72, 0x57}, {0x73, 0x5F},
    {0x31, 0x53},  // backslash: the key left of the ISO-shaped Enter
    // Left-hand 10-key block, top-left to bottom-right: 05 06 / 04 0C / 03 0B /
    // 83 0A / 01 09.
    {0x29, 0x05},  // Esc          -> block row 1 left
    {0x46, 0x06},  // Print Screen -> block row 1 right
    {0x47, 0x04},  // Scroll Lock  -> block row 2 left
    {0x74, 0x0C},  // Execute      -> block row 2 right
    {0x75, 0x03},  // Help         -> block row 3 left
    {0x77, 0x0B},  // Select       -> block row 3 right
    {0x65, 0x83},  // Application  -> block row 4 left
    {0x78, 0x0A},  // Stop         -> block row 4 right
    {0xE3, 0x01},  // Left GUI     -> block row 5 left
    {0xE7, 0x09},  // Right GUI    -> block row 5 right
    {0x85, 0x7B},  // keypad ,     -> keypad row 3 right
    {0x67, 0x78},  // keypad =     -> keypad position under Enter
};

std::array<uint8_t, 256> build_map(KeyboardType type) {
    std::array<uint8_t, 256> m{};
    for (const HidMap& e : kCommon) m[e.hid] = e.code;
    if (type == KeyboardType::Enhanced)
        for (const HidMap& e : kEnhanced) m[e.hid] = e.code;
    else
        for (const HidMap& e : k122) m[e.hid] = e.code;
    return m;
}

constexpr uint8_t kAck = 0xFA;
constexpr uint8_t kResend = 0xFE;
constexpr uint8_t kBatOk = 0xAA;
constexpr uint8_t kEcho = 0xEE;
constexpr uint8_t kBreakPrefix = 0xF0;
constexpr uint8_t kOverrun = 0x00;
constexpr uint16_t kKeyFlag = 0x100;

} // namespace

Set3Keyboard::Set3Keyboard(KeyboardType type, uint8_t id1, uint8_t id2)
    : type_(type), id1_(id1), id2_(id2), bat_sends_id_(type == KeyboardType::Terminal122) {
    reset_defaults();
    set_shift_fkeys(true);
}

void Set3Keyboard::set_shift_fkeys(bool on) {
    shift_fkeys_ = on;
    auto km = std::make_shared<Keymap>();
    builtin_keymap("positional", on, *km);
    keymap_.set_keymap(std::move(km));
}

uint8_t Set3Keyboard::hid_to_set3(KeyboardType type, uint32_t hid_usage) {
    static const std::array<uint8_t, 256> m122 = build_map(KeyboardType::Terminal122);
    static const std::array<uint8_t, 256> menh = build_map(KeyboardType::Enhanced);
    if (hid_usage > 0xFF) return 0;
    return (type == KeyboardType::Enhanced ? menh : m122)[hid_usage];
}

Set3Keyboard::KeyMode Set3Keyboard::default_key_mode(uint8_t code) {
    switch (code) {
    // Shift-type keys send break codes (IBM PS/2 keyboard reference).
    case 0x11: case 0x12: case 0x14: case 0x19: case 0x59:
    case 0x8B: case 0x8C: case 0x8D:
        return MakeBreak;
    // Make-only: Esc/F13, F1-F24, Print Screen, Scroll Lock, Pause, Num Lock,
    // right Alt/Ctrl and the 122-key left-hand block.
    case 0x07: case 0x0F: case 0x17: case 0x1F: case 0x27: case 0x2F:
    case 0x37: case 0x3F: case 0x47: case 0x4F: case 0x56: case 0x5E:
    case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30:
    case 0x38: case 0x40: case 0x48: case 0x50: case 0x57: case 0x5F:
    case 0x62: case 0x76: case 0x39: case 0x58:
    case 0x05: case 0x06: case 0x04: case 0x0C: case 0x03:
    case 0x0B: case 0x83: case 0x0A: case 0x01: case 0x09:
        return MakeOnly;
    default:
        return Typematic;
    }
}

void Set3Keyboard::reset_defaults() {
    for (int c = 0; c < 256; ++c) modes_[size_t(c)] = default_key_mode(uint8_t(c));
    typematic_ = kDefaultTypematic;
    repeat_code_ = -1;
    pending_ = Pending::None;
}

void Set3Keyboard::power_on() {
    out_.clear();
    key_bytes_ = 0;
    overrun_ = false;
    held_.fill(false);
    keymap_.reset();
    leds_ = 0;
    set_ = 3;
    reset_defaults();
    scanning_ = true;
    respond(kBatOk);
    if (bat_sends_id_) {
        respond(id1_);
        respond(id2_);
    }
}

uint8_t Set3Keyboard::read_output() {
    if (out_.empty()) return 0;
    const uint16_t v = out_.front();
    out_.pop_front();
    if (v & kKeyFlag) {
        TN_LOG("input", Debug, "keyboard sends key byte %02X to the terminal", unsigned(v & 0xFF));
        if (key_bytes_) --key_bytes_;
        if (key_bytes_ == 0) overrun_ = false;
    }
    last_sent_ = uint8_t(v);
    return uint8_t(v);
}

void Set3Keyboard::respond(uint8_t b) { out_.push_back(b); }

void Set3Keyboard::clear_key_bytes() {
    std::deque<uint16_t> kept;
    for (uint16_t v : out_)
        if (!(v & kKeyFlag)) kept.push_back(v);
    out_.swap(kept);
    key_bytes_ = 0;
    overrun_ = false;
}

void Set3Keyboard::key_byte(uint8_t b) {
    TN_LOG("input", Trace, "keyboard queues %02X (%zu key bytes waiting)", b, key_bytes_);
    if (overrun_) return;
    if (key_bytes_ >= kBufferLimit) {
        out_.push_back(uint16_t(kKeyFlag | kOverrun));
        ++key_bytes_;
        overrun_ = true;
        return;
    }
    out_.push_back(uint16_t(kKeyFlag | b));
    ++key_bytes_;
}

void Set3Keyboard::set_all(KeyMode m) {
    modes_.fill(m);
    repeat_code_ = -1;
}

void Set3Keyboard::host_write(uint8_t byte) {
    // A command byte always starts a new command, abandoning a parameter or
    // key list in progress (key codes and parameters are all below 0xED).
    if (byte >= 0xED || pending_ == Pending::None) {
        pending_ = Pending::None;
        command(byte);
        return;
    }
    switch (pending_) {
    case Pending::Leds:
        if (byte > 7) { respond(kResend); return; }
        leds_ = byte;
        pending_ = Pending::None;
        respond(kAck);
        break;
    case Pending::ScanSet:
        if (byte > 3) { respond(kResend); return; }
        pending_ = Pending::None;
        respond(kAck);
        if (byte == 0) {
            respond(set_);
        } else {
            set_ = byte;
            if (byte != 3)
                TN_LOG("kbd", Warn, "host selected scan-code set %u; only set 3 is generated", byte);
        }
        break;
    case Pending::Typematic:
        if (byte & 0x80) { respond(kResend); return; }
        typematic_ = byte;
        pending_ = Pending::None;
        respond(kAck);
        break;
    case Pending::KeyList:
        modes_[byte] = list_mode_;
        if (repeat_code_ == byte) repeat_code_ = -1;
        respond(kAck);
        break;
    case Pending::None:
        break;
    }
}

void Set3Keyboard::command(uint8_t cmd) {
    TN_LOG("kbd", Debug, "host command %02X", cmd);
    switch (cmd) {
    case 0xED: respond(kAck); pending_ = Pending::Leds; break;
    case 0xEE: respond(kEcho); break;
    case 0xF0: respond(kAck); pending_ = Pending::ScanSet; break;
    case 0xF2: respond(kAck); respond(id1_); respond(id2_); break;
    case 0xF3: respond(kAck); pending_ = Pending::Typematic; break;
    case 0xF4:  // enable: clear the buffer, start scanning
        clear_key_bytes();
        respond(kAck);
        scanning_ = true;
        break;
    case 0xF5:  // default disable
        clear_key_bytes();
        respond(kAck);
        reset_defaults();
        scanning_ = false;
        break;
    case 0xF6:  // set default
        clear_key_bytes();
        respond(kAck);
        reset_defaults();
        scanning_ = true;
        break;
    case 0xF7: respond(kAck); set_all(Typematic); break;
    case 0xF8: respond(kAck); set_all(MakeBreak); break;
    case 0xF9: respond(kAck); set_all(MakeOnly); break;
    case 0xFA:
        respond(kAck);
        if (type_ == KeyboardType::Terminal122) {
            // The terminal keyboard treats FA exactly as FB: it takes a key
            // list and makes those keys typematic, with no break code
            // (documented for the 1390876 by seasip: "Behaves as FB"; the
            // 3477 firmware sends FA 61 6A for the horizontal cursor keys).
            // The PC Enhanced keyboard sets all keys typematic/make/break.
            list_mode_ = Typematic;
            pending_ = Pending::KeyList;
        } else {
            set_all(TypematicMakeBreak);
        }
        break;
    case 0xFB: respond(kAck); list_mode_ = Typematic; pending_ = Pending::KeyList; break;
    case 0xFC: respond(kAck); list_mode_ = MakeBreak; pending_ = Pending::KeyList; break;
    case 0xFD: respond(kAck); list_mode_ = MakeOnly; pending_ = Pending::KeyList; break;
    case 0xFE: respond(last_sent_); break;
    case 0xFF:
        // Reset: ACK, then the BAT result (power_on() clears the queue).
        power_on();
        out_.push_front(kAck);
        break;
    default:
        TN_LOG("kbd", Warn, "unsupported keyboard command %02X", cmd);
        respond(kResend);
        break;
    }
}

void Set3Keyboard::host_key(const HostKeyEvent& ev) {
    const bool mapped = keymap_.host_key(type_, ev.hid_usage, ev.pressed,
                                         [this](uint8_t code, bool pressed) { key(code, pressed); });
    if (!mapped)
        TN_LOG("kbd", Debug, "host key %s (HID %02X) has no %s mapping", ev.name.c_str(), ev.hid_usage,
               keyboard_type_name(type_));
}

bool Set3Keyboard::terminal_key(const std::string& function, bool pressed) {
    std::array<TerminalStroke, 2> to{};
    std::array<bool, 2> ok{};
    const size_t ti = type_ == KeyboardType::Enhanced ? 1 : 0;
    if (!terminal_target_from_name(function, to, ok) || !ok[ti] || to[ti].code == 0) return false;
    TN_LOG("input", Debug, "terminal function %s %s (set-3 %02X)", function.c_str(), pressed ? "press" : "release",
           to[ti].code);
    keymap_.stroke(to[ti], pressed, [this](uint8_t code, bool down) { key(code, down); });
    return true;
}

bool Set3Keyboard::set_host_keymap(const std::string& spec, bool shift_fkeys, std::string& error) {
    auto km = std::make_shared<Keymap>();
    if (!load_keymap(spec, shift_fkeys, *km, error)) return false;
    shift_fkeys_ = shift_fkeys;
    keymap_.set_keymap(std::move(km));
    return true;
}

namespace {
const char* key_mode_name(uint8_t m) {
    switch (m & 3) {
    case 0: return "make-only";
    case 1: return "typematic";
    case 2: return "make/break";
    default: return "typematic make/break";
    }
}
} // namespace

void Set3Keyboard::key(uint8_t code, bool pressed) {
    TN_LOG("input", Debug, "terminal key %02X %s (%s%s%s)", code, pressed ? "make" : "break",
           key_mode_name(modes_[code]), scanning_ ? "" : ", not scanning",
           held_[code] == pressed ? ", already in that state" : "");
    if (pressed) {
        if (held_[code]) return;  // host auto-repeat; the keyboard repeats itself
        held_[code] = true;
        if (!scanning_) return;
        key_byte(code);
        if (modes_[code] & Typematic) {
            repeat_code_ = code;
            repeat_wait_us_ = typematic_delay_us();
        } else {
            repeat_code_ = -1;
        }
    } else {
        if (!held_[code]) return;
        held_[code] = false;
        if (repeat_code_ == code) repeat_code_ = -1;
        if (!scanning_) return;
        if (modes_[code] & MakeBreak) {
            key_byte(kBreakPrefix);
            key_byte(code);
        }
    }
}

uint64_t Set3Keyboard::typematic_delay_us() const {
    return uint64_t(1 + ((typematic_ >> 5) & 3)) * 250000u;
}

uint64_t Set3Keyboard::typematic_period_us() const {
    // Period = (8 + A) * 2^B * 4.17 ms, A = bits 2-0, B = bits 4-3.
    const uint64_t a = typematic_ & 7, b = (typematic_ >> 3) & 3;
    return (8 + a) * (uint64_t(1) << b) * 4170u;
}

void Set3Keyboard::advance_us(uint64_t us) {
    while (repeat_code_ >= 0 && scanning_) {
        if (us < repeat_wait_us_) {
            repeat_wait_us_ -= us;
            return;
        }
        us -= repeat_wait_us_;
        TN_LOG("input", Debug, "terminal key %02X typematic repeat", repeat_code_);
        key_byte(uint8_t(repeat_code_));
        repeat_wait_us_ = typematic_period_us();
    }
}

} // namespace em34xx
