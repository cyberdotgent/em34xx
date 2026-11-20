#include "host/wsc/keyboard_map.hpp"

#include <cstddef>

namespace tn34xx {
namespace wsc {

namespace {

// IBM Enhanced Keyboard, PAI fig. 147: scan code -> US unshifted / shifted
// character, as EBCDIC CCSID 37. 0 = not a character key.
struct CharKey {
    uint8_t scan;
    uint8_t lower;
    uint8_t upper;
    bool letter;   // affected by Caps Lock
};

constexpr CharKey kEnhancedUs[] = {
    {0x0E, 0x79, 0xA1, false},   // ` ~
    {0x16, 0xF1, 0x5A, false},   // 1 !
    {0x1E, 0xF2, 0x7C, false},   // 2 @
    {0x26, 0xF3, 0x7B, false},   // 3 #
    {0x25, 0xF4, 0x5B, false},   // 4 $
    {0x2E, 0xF5, 0x6C, false},   // 5 %
    {0x36, 0xF6, 0x5F, false},   // 6 (shift: logical not; the figure is illegible here) [hyp]
    {0x3D, 0xF7, 0x50, false},   // 7 &
    {0x3E, 0xF8, 0x5C, false},   // 8 *
    {0x46, 0xF9, 0x4D, false},   // 9 (
    {0x45, 0xF0, 0x5D, false},   // 0 )
    {0x4E, 0x60, 0x6D, false},   // - _
    {0x55, 0x7E, 0x4E, false},   // = +
    {0x15, 0x98, 0xD8, true},    // q
    {0x1D, 0xA6, 0xE6, true},    // w
    {0x24, 0x85, 0xC5, true},    // e
    {0x2D, 0x99, 0xD9, true},    // r
    {0x2C, 0xA3, 0xE3, true},    // t
    {0x35, 0xA8, 0xE8, true},    // y
    {0x3C, 0xA4, 0xE4, true},    // u
    {0x43, 0x89, 0xC9, true},    // i
    {0x44, 0x96, 0xD6, true},    // o
    {0x4D, 0x97, 0xD7, true},    // p
    {0x54, 0xBA, 0xC0, false},   // [ {
    {0x5B, 0xBB, 0xD0, false},   // ] }
    {0x5C, 0xE0, 0x4F, false},   // \ |
    {0x1C, 0x81, 0xC1, true},    // a
    {0x1B, 0xA2, 0xE2, true},    // s
    {0x23, 0x84, 0xC4, true},    // d
    {0x2B, 0x86, 0xC6, true},    // f
    {0x34, 0x87, 0xC7, true},    // g
    {0x33, 0x88, 0xC8, true},    // h
    {0x3B, 0x91, 0xD1, true},    // j
    {0x42, 0x92, 0xD2, true},    // k
    {0x4B, 0x93, 0xD3, true},    // l
    {0x4C, 0x5E, 0x7A, false},   // ; :
    {0x52, 0x7D, 0x7F, false},   // ' "
    {0x1A, 0xA9, 0xE9, true},    // z
    {0x22, 0xA7, 0xE7, true},    // x
    {0x21, 0x83, 0xC3, true},    // c
    {0x2A, 0xA5, 0xE5, true},    // v
    {0x32, 0x82, 0xC2, true},    // b
    {0x31, 0x95, 0xD5, true},    // n
    {0x3A, 0x94, 0xD4, true},    // m
    {0x41, 0x6B, 0x4C, false},   // , <
    {0x49, 0x4B, 0x6E, false},   // . >
    {0x4A, 0x61, 0x6F, false},   // / ?
    {0x29, 0x40, 0x40, false},   // space
    // Numeric pad: the same character shifted or not.
    {0x6C, 0xF7, 0xF7, false}, {0x75, 0xF8, 0xF8, false}, {0x7D, 0xF9, 0xF9, false},
    {0x6B, 0xF4, 0xF4, false}, {0x73, 0xF5, 0xF5, false}, {0x74, 0xF6, 0xF6, false},
    {0x69, 0xF1, 0xF1, false}, {0x72, 0xF2, 0xF2, false}, {0x7A, 0xF3, 0xF3, false},
    {0x70, 0xF0, 0xF0, false},
    {0x71, 0x4B, 0x4B, false},   // . (pad)
    {0x7E, 0x6B, 0x6B, false},   // , (pad)
};

// 122-key typewriter keyboard, US, keyed by key position (set-3 code; the
// cable carries other codes, decoded by kTypewriterCable below). The typing
// block shares its positions with the Enhanced keyboard. Differences: the backslash key sits left of Enter (53h, not 5Ch),
// and the keypad has no '/' or '*'. US graphics as fig. 147 [hyp: the 122-key
// US keytops carry the same CCSID 37 graphics].
constexpr CharKey kTypewriterUs[] = {
    {0x0E, 0x79, 0xA1, false},   // ` ~
    {0x16, 0xF1, 0x5A, false}, {0x1E, 0xF2, 0x7C, false}, {0x26, 0xF3, 0x7B, false},
    {0x25, 0xF4, 0x5B, false}, {0x2E, 0xF5, 0x6C, false}, {0x36, 0xF6, 0x5F, false},
    {0x3D, 0xF7, 0x50, false}, {0x3E, 0xF8, 0x5C, false}, {0x46, 0xF9, 0x4D, false},
    {0x45, 0xF0, 0x5D, false}, {0x4E, 0x60, 0x6D, false}, {0x55, 0x7E, 0x4E, false},
    {0x15, 0x98, 0xD8, true}, {0x1D, 0xA6, 0xE6, true}, {0x24, 0x85, 0xC5, true},
    {0x2D, 0x99, 0xD9, true}, {0x2C, 0xA3, 0xE3, true}, {0x35, 0xA8, 0xE8, true},
    {0x3C, 0xA4, 0xE4, true}, {0x43, 0x89, 0xC9, true}, {0x44, 0x96, 0xD6, true},
    {0x4D, 0x97, 0xD7, true},
    {0x54, 0xBA, 0xC0, false},   // [ {
    {0x5B, 0xBB, 0xD0, false},   // ] }
    {0x53, 0xE0, 0x4F, false},   // \ | (left of Enter on the 122-key)
    {0x1C, 0x81, 0xC1, true}, {0x1B, 0xA2, 0xE2, true}, {0x23, 0x84, 0xC4, true},
    {0x2B, 0x86, 0xC6, true}, {0x34, 0x87, 0xC7, true}, {0x33, 0x88, 0xC8, true},
    {0x3B, 0x91, 0xD1, true}, {0x42, 0x92, 0xD2, true}, {0x4B, 0x93, 0xD3, true},
    {0x4C, 0x5E, 0x7A, false},   // ; :
    {0x52, 0x7D, 0x7F, false},   // ' "
    {0x1A, 0xA9, 0xE9, true}, {0x22, 0xA7, 0xE7, true}, {0x21, 0x83, 0xC3, true},
    {0x2A, 0xA5, 0xE5, true}, {0x32, 0x82, 0xC2, true}, {0x31, 0x95, 0xD5, true},
    {0x3A, 0x94, 0xD4, true},
    {0x41, 0x6B, 0x4C, false},   // , <
    {0x49, 0x4B, 0x6E, false},   // . >
    {0x4A, 0x61, 0x6F, false},   // / ?
    {0x29, 0x40, 0x40, false},   // space
    // Numeric pad.
    {0x6C, 0xF7, 0xF7, false}, {0x75, 0xF8, 0xF8, false}, {0x7D, 0xF9, 0xF9, false},
    {0x6B, 0xF4, 0xF4, false}, {0x73, 0xF5, 0xF5, false}, {0x74, 0xF6, 0xF6, false},
    {0x69, 0xF1, 0xF1, false}, {0x72, 0xF2, 0xF2, false}, {0x7A, 0xF3, 0xF3, false},
    {0x70, 0xF0, 0xF0, false},
    {0x71, 0x4B, 0x4B, false},   // . (pad)
};

struct FnKey {
    uint8_t scan;
    OperatorAction::Kind kind;   // Function or Aid (unshifted)
    KeyFunction function;
    uint8_t aid;
    OperatorAction::Kind shift_kind;
    KeyFunction shift_function;
    uint8_t shift_aid;
};

using K = OperatorAction::Kind;
using F = KeyFunction;

constexpr FnKey kEnhancedFunctions[] = {
    {0x66, K::Function, F::Backspace, 0, K::Function, F::Backspace, 0},
    {0x0D, K::Function, F::Tab, 0, K::Function, F::Backtab, 0},
    {0x7C, K::Function, F::Tab, 0, K::Function, F::Tab, 0},          // right tab (pad)
    {0x5A, K::Function, F::FieldExit, 0, K::Function, F::NewLine, 0},
    {0x7B, K::Function, F::FieldPlus, 0, K::Function, F::FieldPlus, 0},
    {0x7F, K::Function, F::FieldMinus, 0, K::Function, F::FieldMinus, 0},
    {0x11, K::Function, F::Reset, 0, K::Function, F::Reset, 0},
    {0x58, K::Aid, F::None, 0xF1, K::Aid, F::None, 0xF1},             // Enter
    {0x79, K::Aid, F::None, 0xF1, K::Aid, F::None, 0xF1},             // Enter (pad)
    {0x67, K::Function, F::Insert, 0, K::Function, F::Dup, 0},
    {0x64, K::Function, F::Delete, 0, K::Function, F::Delete, 0},
    {0x61, K::Function, F::CursorLeft, 0, K::Function, F::CursorLeft, 0},
    {0x6A, K::Function, F::CursorRight, 0, K::Function, F::CursorRight, 0},
    {0x63, K::Function, F::CursorUp, 0, K::Function, F::CursorUp, 0},
    {0x60, K::Function, F::CursorDown, 0, K::Function, F::CursorDown, 0},
    {0x6E, K::Function, F::Home, 0, K::Function, F::Home, 0},
    {0x65, K::Function, F::EraseEof, 0, K::Function, F::EraseEof, 0},  // End position; unshifted meaning [hyp]
    {0x6F, K::Aid, F::None, 0xF4, K::Aid, F::None, 0xF4},             // Page Up = Roll Down
    {0x6D, K::Aid, F::None, 0xF5, K::Aid, F::None, 0xF5},             // Page Down = Roll Up
    {0x57, K::Aid, F::None, 0xF6, K::Aid, F::None, 0xF6},             // Print
    {0x5F, K::Aid, F::None, 0xF3, K::Function, F::Test, 0},           // Help / Test
    {0x62, K::Aid, F::None, 0xBD, K::Aid, F::None, 0xBD},             // Clear
    {0x08, K::Attn, F::None, 0, K::Function, F::SetCp, 0},            // Attn / Set CP
    // F1..F12 / F13..F24 [doc FR 16.2.1: AID 31-3C, B1-BC]
    {0x07, K::Aid, F::None, 0x31, K::Aid, F::None, 0xB1},
    {0x0F, K::Aid, F::None, 0x32, K::Aid, F::None, 0xB2},
    {0x17, K::Aid, F::None, 0x33, K::Aid, F::None, 0xB3},
    {0x1F, K::Aid, F::None, 0x34, K::Aid, F::None, 0xB4},
    {0x27, K::Aid, F::None, 0x35, K::Aid, F::None, 0xB5},
    {0x2F, K::Aid, F::None, 0x36, K::Aid, F::None, 0xB6},
    {0x37, K::Aid, F::None, 0x37, K::Aid, F::None, 0xB7},
    {0x3F, K::Aid, F::None, 0x38, K::Aid, F::None, 0xB8},
    {0x47, K::Aid, F::None, 0x39, K::Aid, F::None, 0xB9},
    {0x4F, K::Aid, F::None, 0x3A, K::Aid, F::None, 0xBA},
    {0x56, K::Aid, F::None, 0x3B, K::Aid, F::None, 0xBB},
    {0x5E, K::Aid, F::None, 0x3C, K::Aid, F::None, 0xBC},
};

// ALT combinations from fig. 147's ALT column.
bool alt_action(uint8_t scan, OperatorAction& out) {
    switch (scan) {
    case 0x57: out.kind = K::SysReq; return true;                            // Sys Req
    case 0x65: out.kind = K::Function; out.function = F::EraseInput; return true;
    case 0x1F: out.kind = K::Function; out.function = F::Record; return true;   // Alt+F4
    case 0x27: out.kind = K::Function; out.function = F::Play; return true;     // Alt+F5
    case 0x37: out.kind = K::Function; out.function = F::Hex; return true;      // Alt+F7
    default: return false;
    }
}


// 122-key typewriter functions, keyed by key position (the keyboard's set-3
// code; the cable code is decoded to it first, see kTypewriterCable). Legends are
// from the 3489 User's Guide (GA18-7492) key tables, which share this
// keyboard, placed with a photo of the 1394324 keyboard and the TMK set-3
// position map (docs/keyboard.md), and from the 3477 firmware's own
// handling of the local-function keys.
constexpr FnKey kTypewriterFunctions[] = {
    {0x66, K::Function, F::Backspace, 0, K::Function, F::Backspace, 0},
    {0x0D, K::Function, F::Tab, 0, K::Function, F::Backtab, 0},         // Field Advance / Field Backspace
    {0x5A, K::Function, F::FieldExit, 0, K::Function, F::FieldExit, 0},
    {0x58, K::Aid, F::None, 0xF1, K::Aid, F::None, 0xF1},             // Enter (right of the space bar)
    {0x11, K::Function, F::Reset, 0, K::Function, F::Reset, 0},         // Reset (Alt: Quit, local)
    // Editing block above the cursor keys: Field Backspace, Dup, (blank;
    // Alt: Jump) / New Line, Insert, Delete.
    {0x67, K::Function, F::Backtab, 0, K::Function, F::Backtab, 0},
    {0x6E, K::Function, F::Dup, 0, K::Function, F::Dup, 0},
    {0x64, K::Function, F::NewLine, 0, K::Function, F::NewLine, 0},
    {0x65, K::Function, F::Insert, 0, K::Function, F::Insert, 0},
    {0x6D, K::Function, F::Delete, 0, K::Function, F::Delete, 0},
    {0x61, K::Function, F::CursorLeft, 0, K::Function, F::CursorLeft, 0},
    {0x6A, K::Function, F::CursorRight, 0, K::Function, F::CursorRight, 0},
    {0x63, K::Function, F::CursorUp, 0, K::Aid, F::None, 0xF5},         // shift: Roll Up
    {0x60, K::Function, F::CursorDown, 0, K::Aid, F::None, 0xF4},       // shift: Roll Down
    // Keypad right column: Field- (row 2), blank (row 3), Field+ (rows 4-5).
    // The top row (76 77 7E, and set-3 84h, which arrives as 7Fh) is blank.
    {0x7C, K::Function, F::FieldMinus, 0, K::Function, F::FieldMinus, 0},
    {0x79, K::Function, F::FieldPlus, 0, K::Function, F::FieldPlus, 0},
    // Left-hand block.
    {0x05, K::Attn, F::None, 0, K::SysReq, F::None, 0},               // Attn / SysRq
    {0x0C, K::Function, F::EraseInput, 0, K::Function, F::EraseInput, 0},
    {0x0B, K::Aid, F::None, 0xF3, K::Aid, F::None, 0xF3},             // Help (Alt: Hex, CMD 3Eh)
    {0x03, K::Aid, F::None, 0xF6, K::Aid, F::None, 0xF6},             // Print
    {0x09, K::Function, F::Record, 0, K::Function, F::Record, 0},       // Recrd (terminal-local)
    {0x0A, K::Function, F::Play, 0, K::Function, F::Play, 0},           // Play (terminal-local; Alt: Test)
};

// 122-key typewriter keyboard on the cable: the terminal sends the 3180-2 /
// 3196 scan codes [doc PAI fig. 105, key numbers placed by fig. 103; observed:
// the 3477 sends exactly these for every position, docs/keyboard.md]. They are
// 5251-compatible: one code per key, Shift as make/break codes 57h/56h (the
// terminal synthesises them where a key needs a given shift state), Shift
// Lock 54h, and a CMD prefix 6Fh instead of Alt and for F1-F24. This table
// takes a code back to the key position (set-3 code) the legends are keyed by.
struct CableKey {
    uint8_t code;
    uint8_t position;
};

constexpr CableKey kTypewriterCable[] = {
    // Left-hand block (01 SetUp, 09 Record, 0A Play, and 03 Print when local
    // printing is configured, are kept by the terminal).
    {0x7C, 0x05}, {0x66, 0x04}, {0x6E, 0x03}, {0x78, 0x83}, {0x75, 0x06}, {0x67, 0x0C}, {0x7D, 0x0B},
    // Typing block: digit row, then Q, A and Z rows.
    {0x3E, 0x0E}, {0x31, 0x16}, {0x32, 0x1E}, {0x33, 0x26}, {0x34, 0x25}, {0x35, 0x2E}, {0x36, 0x36},
    {0x37, 0x3D}, {0x38, 0x3E}, {0x39, 0x46}, {0x3A, 0x45}, {0x3B, 0x4E}, {0x3C, 0x55}, {0x3D, 0x66},
    {0x20, 0x0D}, {0x21, 0x15}, {0x22, 0x1D}, {0x23, 0x24}, {0x24, 0x2D}, {0x25, 0x2C}, {0x26, 0x35},
    {0x27, 0x3C}, {0x28, 0x43}, {0x29, 0x44}, {0x2A, 0x4D}, {0x2B, 0x54}, {0x2C, 0x5B}, {0x2D, 0x5A},
    {0x11, 0x1C}, {0x12, 0x1B}, {0x13, 0x23}, {0x14, 0x2B}, {0x15, 0x34}, {0x16, 0x33}, {0x17, 0x3B},
    {0x18, 0x42}, {0x19, 0x4B}, {0x1A, 0x4C}, {0x1B, 0x52}, {0x1C, 0x53},
    {0x0E, 0x13}, {0x01, 0x1A}, {0x02, 0x22}, {0x03, 0x21}, {0x04, 0x2A}, {0x05, 0x32}, {0x06, 0x31},
    {0x07, 0x3A}, {0x08, 0x41}, {0x09, 0x49}, {0x0A, 0x4A},
    // Bottom row: Reset, space bar, Enter.
    {0x7E, 0x11}, {0x0F, 0x29}, {0x68, 0x58},
    // Editing and cursor block. 6Ch is Insert in lower shift and Delete in
    // upper shift (decoded in translate()); 60h is Rule, kept by the terminal.
    {0x4B, 0x67}, {0x4C, 0x6E}, {0x62, 0x6F}, {0x0C, 0x64},
    {0x71, 0x63}, {0x72, 0x61}, {0x73, 0x6A}, {0x70, 0x60}, {0x60, 0x62},
    // Numeric pad.
    {0x1D, 0x76}, {0x1E, 0x77}, {0x4F, 0x7E}, {0x50, 0x84},
    {0x47, 0x6C}, {0x48, 0x75}, {0x49, 0x7D}, {0x4E, 0x7C},
    {0x44, 0x6B}, {0x45, 0x73}, {0x46, 0x74}, {0x55, 0x7B},
    {0x41, 0x69}, {0x42, 0x72}, {0x43, 0x7A}, {0x4D, 0x79},
    {0x40, 0x70}, {0x4A, 0x71},
};

uint8_t typewriter_position(uint8_t code, bool upper) {
    if (code == 0x6C) return upper ? 0x6D : 0x65;   // Delete / Insert
    for (const CableKey& k : kTypewriterCable)
        if (k.code == code) return k.position;
    return 0;
}

template <size_t N>
bool lookup(const FnKey (&table)[N], uint8_t key, bool upper, OperatorAction& out) {
    for (const FnKey& f : table) {
        if (f.scan != key) continue;
        out.kind = upper ? f.shift_kind : f.kind;
        out.function = upper ? f.shift_function : f.function;
        out.aid = upper ? f.shift_aid : f.aid;
        return true;
    }
    return false;
}

} // namespace

const char* key_function_name(KeyFunction f) {
    switch (f) {
    case F::None: return "none";
    case F::FieldExit: return "field-exit";
    case F::FieldPlus: return "field+";
    case F::FieldMinus: return "field-";
    case F::NewLine: return "new-line";
    case F::Tab: return "tab";
    case F::Backtab: return "backtab";
    case F::Backspace: return "backspace";
    case F::CursorUp: return "up";
    case F::CursorDown: return "down";
    case F::CursorLeft: return "left";
    case F::CursorRight: return "right";
    case F::Home: return "home";
    case F::Insert: return "insert";
    case F::Delete: return "delete";
    case F::Dup: return "dup";
    case F::EraseEof: return "erase-eof";
    case F::EraseInput: return "erase-input";
    case F::Reset: return "reset";
    case F::FieldMark: return "field-mark";
    case F::Hex: return "hex";
    case F::Record: return "record";
    case F::Play: return "play";
    case F::Test: return "test";
    case F::SetCp: return "set-cp";
    }
    return "?";
}

void KeyboardMap::reset() {
    lshift_ = rshift_ = alt_ = caps_ = cmd_ = false;
}

bool KeyboardMap::translate(uint8_t scan, OperatorAction& out) {
    out = OperatorAction{};
    if (scan == 0xFF) {
        out.kind = K::Overrun;
        out.scan = scan;
        return true;
    }
    const bool brk = (scan & 0x80) != 0;
    const uint8_t key = uint8_t(scan & 0x7F);
    out.scan = key;

    if (!enhanced()) return translate_typewriter(key, brk, out);

    // Make/break keys (fig. 147 "MB"): shifts, Alt, Caps Lock.
    switch (key) {
    case 0x12: case 0x59:
        (key == 0x12 ? lshift_ : rshift_) = !brk;
        return false;
    case 0x19: case 0x39: alt_ = !brk; return false;
    case 0x14:   // Caps Lock toggles
        if (!brk) caps_ = !caps_;
        return false;
    default: break;
    }
    if (brk) return false;   // only shift-type keys report a break

    out.shift = shift();
    out.alt = alt_;
    if (alt_ && alt_action(key, out)) return true;
    if (lookup(kEnhancedFunctions, key, shift(), out)) return true;
    for (const CharKey& c : kEnhancedUs) {
        if (c.scan != key) continue;
        const bool upper = c.letter ? (shift() != caps_) : shift();
        out.kind = K::Character;
        out.ebcdic = upper ? c.upper : c.lower;
        return true;
    }
    return true;   // Unmapped
}

bool KeyboardMap::translate_typewriter(uint8_t key, bool brk, OperatorAction& out) {
    // Make/break keys [doc PAI fig. 105 "MB"]: left Shift 57h, right Shift
    // 56h, Shift Lock 54h. Alt is not sent; it turns into the CMD prefix.
    switch (key) {
    case 0x57: case 0x56:
        (key == 0x57 ? lshift_ : rshift_) = !brk;
        if (!brk) caps_ = false;   // a Shift key releases Shift Lock
        return false;
    case 0x54:
        if (!brk) caps_ = true;
        return false;
    default: break;
    }
    if (brk) return false;
    if (key == 0x6F) {   // CMD: modifies the next key
        cmd_ = true;
        return false;
    }
    const bool cmd = cmd_;
    cmd_ = false;
    const bool upper = shift() || caps_;
    out.shift = shift();
    out.alt = cmd;

    if (cmd) {
        // CMD + top-row code: F1-F12, F13-F24 in upper shift [doc PAI fig.
        // 105 keys 110-133, notes 3 and 4].
        if (key >= 0x31 && key <= 0x3C) {
            out.kind = K::Aid;
            out.aid = uint8_t((upper ? 0xB1 : 0x31) + (key - 0x31));
            return true;
        }
        switch (key) {
        case 0x3E: out.kind = K::Function; out.function = F::Hex; return true;   // Alt+Help
        case 0x3D:   // Alt+Clear (upper shift) / Alt+Test (lower shift)
            if (upper) {
                out.kind = K::Aid;
                out.aid = 0xBD;
            } else {
                out.kind = K::Function;
                out.function = F::Test;
            }
            return true;
        case 0x51: return true;   // Alt + digit-row keys: no function (Unmapped)
        default: break;
        }
    }
    if (key == 0x6D) {   // Alt+Rule: Home [doc PAI fig. 105 key 83]
        out.kind = K::Function;
        out.function = F::Home;
        return true;
    }
    const uint8_t pos = typewriter_position(key, upper);
    if (!pos) return true;   // Unmapped
    if (lookup(kTypewriterFunctions, pos, upper, out)) return true;
    for (const CharKey& c : kTypewriterUs) {
        if (c.scan != pos) continue;
        out.kind = K::Character;
        out.ebcdic = upper ? c.upper : c.lower;
        return true;
    }
    return true;   // Unmapped: blank keytops (04 06 83 6F 76 77 7E 84 7B)
}

} // namespace wsc
} // namespace tn34xx
