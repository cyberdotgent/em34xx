#include "host/wsc/keyboard_map.hpp"

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
    lshift_ = rshift_ = alt_ = caps_ = false;
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

    // Make/break keys (fig. 147 "MB"): shifts, Alt, Caps Lock.
    switch (key) {
    case 0x12: lshift_ = !brk; return false;
    case 0x59: rshift_ = !brk; return false;
    case 0x19: case 0x39: alt_ = !brk; return false;
    case 0x14: if (!brk) caps_ = !caps_; return false;
    default: break;
    }
    if (brk) return false;   // only shift-type keys report a break

    out.shift = shift();
    out.alt = alt_;
    if (!enhanced()) return true;   // typewriter table not typed in yet: Unmapped

    if (alt_ && alt_action(key, out)) return true;
    for (const FnKey& f : kEnhancedFunctions) {
        if (f.scan != key) continue;
        out.kind = shift() ? f.shift_kind : f.kind;
        out.function = shift() ? f.shift_function : f.function;
        out.aid = shift() ? f.shift_aid : f.aid;
        return true;
    }
    for (const CharKey& c : kEnhancedUs) {
        if (c.scan != key) continue;
        const bool upper = c.letter ? (shift() != caps_) : shift();
        out.kind = K::Character;
        out.ebcdic = upper ? c.upper : c.lower;
        return true;
    }
    return true;   // Unmapped
}

} // namespace wsc
} // namespace tn34xx
