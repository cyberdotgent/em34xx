// SPDX-License-Identifier: GPL-3.0-or-later
#include "input/host_keymap.hpp"

#include "util/log.hpp"
#include "util/strings.hpp"

#include <toml++/toml.hpp>

#include <fstream>
#include <iterator>
#include <sstream>

namespace em34xx {

namespace {

constexpr ModAction K = ModAction::Keep;
constexpr ModAction P = ModAction::Press;
constexpr ModAction R = ModAction::Release;

int type_index(KeyboardType t) { return t == KeyboardType::Enhanced ? 1 : 0; }

// Names are compared lowercased with '-', '_' and spaces removed.
std::string normalise(const std::string& s) {
    std::string out;
    for (char c : to_lower(trim(s)))
        if (c != '-' && c != '_' && c != ' ') out += c;
    return out;
}

// --- Host keys (USB HID usage page 7) -------------------------------------

struct HostName {
    const char* name;
    uint32_t hid;
};

const HostName kHostNames[] = {
    {"enter", 0x28}, {"return", 0x28}, {"escape", 0x29}, {"esc", 0x29}, {"backspace", 0x2A},
    {"tab", 0x2B}, {"space", 0x2C}, {"minus", 0x2D}, {"equals", 0x2E}, {"leftbracket", 0x2F},
    {"rightbracket", 0x30}, {"backslash", 0x31}, {"nonushash", 0x32}, {"semicolon", 0x33},
    {"apostrophe", 0x34}, {"grave", 0x35}, {"comma", 0x36}, {"period", 0x37}, {"slash", 0x38},
    {"capslock", 0x39}, {"printscreen", 0x46}, {"prtsc", 0x46}, {"print", 0x46},
    {"scrolllock", 0x47}, {"pause", 0x48}, {"break", 0x48}, {"insert", 0x49}, {"ins", 0x49},
    {"home", 0x4A}, {"pageup", 0x4B}, {"pgup", 0x4B}, {"delete", 0x4C}, {"del", 0x4C},
    {"end", 0x4D}, {"pagedown", 0x4E}, {"pgdn", 0x4E}, {"right", 0x4F}, {"left", 0x50},
    {"down", 0x51}, {"up", 0x52}, {"numlock", 0x53}, {"kpdivide", 0x54}, {"kpslash", 0x54},
    {"kpmultiply", 0x55}, {"kpstar", 0x55}, {"kpminus", 0x56}, {"kpplus", 0x57},
    {"kpenter", 0x58}, {"kpperiod", 0x63}, {"kpdecimal", 0x63}, {"nonusbackslash", 0x64},
    {"application", 0x65}, {"menu", 0x65}, {"kpequals", 0x67}, {"execute", 0x74},
    {"help", 0x75}, {"select", 0x77}, {"stop", 0x78}, {"kpcomma", 0x85},
    {"international1", 0x87}, {"international2", 0x88}, {"international3", 0x89},
    {"international4", 0x8A}, {"international5", 0x8B},
    {"lctrl", 0xE0}, {"leftctrl", 0xE0}, {"lshift", 0xE1}, {"leftshift", 0xE1},
    {"lalt", 0xE2}, {"leftalt", 0xE2}, {"lgui", 0xE3}, {"leftgui", 0xE3},
    {"rctrl", 0xE4}, {"rightctrl", 0xE4}, {"rshift", 0xE5}, {"rightshift", 0xE5},
    {"ralt", 0xE6}, {"rightalt", 0xE6}, {"altgr", 0xE6}, {"rgui", 0xE7}, {"rightgui", 0xE7},
};

// --- Terminal functions ---------------------------------------------------
//
// Legends of the U.S. 122-key typewriter keyboard and IBM Enhanced keyboard
// in the IBM InfoWindow II 3489 User's Guide (GA18-7492-00, figures 4-1 and
// 4-3; documented for the 3489, assumed for the 3477). An upper legend is
// entered with Shift, a front legend with Alt (same guide, 4.2.1). Set-3
// codes of the positions follow the TMK 122-key diagram (docs/keyboard.md).
// Codes 7B (122-key keypad row 3 right, Enhanced Field +) and 7C on the
// Enhanced keypad are inferred from the figure's key shapes.

struct Fn {
    const char* name;
    TerminalStroke t122;
    TerminalStroke tenh;
};

const Fn kFunctions[] = {
    {"attn",        {0x05, R, R}, {0x08, R, R}},
    {"sysrq",       {0x05, P, R}, {0x57, R, P}},
    {"setup",       {0x01, R, R}, {0x08, P, R}},
    {"clear",       {0x06, R, P}, {0x62, R, P}},
    {"erinp",       {0x0C, R, R}, {0x65, R, P}},
    {"print",       {0x03, R, R}, {0x57, R, R}},
    {"help",        {0x0B, R, R}, {0x5F, R, R}},
    {"hex",         {0x0B, R, P}, {0x37, R, P}},
    {"test",        {0x0A, R, P}, {0x5F, R, P}},
    {"play",        {0x0A, R, R}, {0x27, R, P}},
    {"record",      {0x09, R, R}, {0x1F, R, P}},
    {"pause",       {0x09, R, P}, {0x62, R, R}},
    {"reset",       {0x11, R, R}, {0x11, R, R}},
    {"quit",        {0x11, R, P}, {0x11, R, P}},
    {"enter",       {0x58, R, R}, {0x58, R, R}},
    {"fieldexit",   {0x5A, R, R}, {0x5A, R, R}},
    {"newline",     {0x64, R, R}, {0x5A, P, R}},
    {"fieldplus",   {0x79, R, R}, {0x7B, R, R}},
    {"fieldminus",  {0x7C, R, R}, {0x84, R, R}},
    {"dup",         {0x6E, R, R}, {0x67, P, R}},
    {"insert",      {0x65, R, R}, {0x67, R, R}},
    {"delete",      {0x6D, R, R}, {0x64, R, R}},
    {"backtab",     {0x67, R, R}, {0x0D, P, R}},
    {"tab",         {0x0D, R, R}, {0x0D, R, R}},
    {"changescreen",{0x67, R, P}, {0x67, R, P}},
    {"jump",        {0x6F, R, P}, {0x6F, R, P}},   // 122-key: Alt + the blank 6F key
    {"rule",        {0x62, R, R}, {0x6D, R, P}},
    {"home",        {0x62, R, P}, {0x6E, R, R}},
    {"rollup",      {0x63, P, R}, {0x6D, R, R}},
    {"rolldown",    {0x60, P, R}, {0x6F, R, R}},
    {"up",          {0x63, R, R}, {0x63, R, R}},
    {"down",        {0x60, R, R}, {0x60, R, R}},
    {"left",        {0x61, R, R}, {0x61, R, R}},
    {"right",       {0x6A, R, R}, {0x6A, R, R}},
    {"backspace",   {0x66, R, R}, {0x66, R, R}},
    {"back",        {0x47, R, P}, {0x47, R, P}},
    {"fwd",         {0x4F, R, P}, {0x4F, R, P}},
};

const struct { const char* alias; const char* name; } kFunctionAliases[] = {
    {"attention", "attn"}, {"sysreq", "sysrq"}, {"systemrequest", "sysrq"},
    {"eraseinput", "erinp"}, {"hostprint", "print"}, {"testrequest", "test"},
    {"fieldbackspace", "backtab"},
    {"fieldadvance", "tab"}, {"rollupcursor", "rollup"}, {"pageup", "rolldown"},
    {"pagedown", "rollup"}, {"duplicate", "dup"},
};

// The 3270 legends of the 122-key keyboard, for the coax terminals (3483,
// 3472) [documented: IBM Personal Communications/3270 V4.0 Reference Guide
// for Windows, S20H-1773, 5.1.1, "PS/2 122 Key Keyboard"; hypothesis: the
// 3483's and 3472's keyboards carry the same]: names with the prefix
// "3270:". The coax controller decodes these positions the same way
// (host/coax/cut_controller.cpp). No Enhanced keyboard strokes.
const Fn kFunctions3270[] = {
    {"attn",      {0x05, R, R}, {}},
    {"sysrq",     {0x05, R, P}, {}},
    {"clear",     {0x06, R, R}, {}},
    {"erinp",     {0x0C, R, R}, {}},
    {"ereof",     {0x0B, R, R}, {}},
    {"pa1",       {0x67, R, R}, {}},
    {"pa2",       {0x6E, R, R}, {}},
    {"pa3",       {0x6F, R, P}, {}},
    {"dup",       {0x67, P, R}, {}},
    {"fieldmark", {0x6E, P, R}, {}},
    {"backtab",   {0x64, R, R}, {}},
    {"insert",    {0x65, R, R}, {}},
    {"delete",    {0x6D, R, R}, {}},
    {"home",      {0x62, R, P}, {}},
    {"rule",      {0x62, R, R}, {}},
    {"up",        {0x63, R, R}, {}},
    {"down",      {0x60, R, R}, {}},
    {"left",      {0x61, R, R}, {}},
    {"right",     {0x6A, R, R}, {}},
    {"backspace", {0x66, R, R}, {}},
    {"tab",       {0x0D, R, R}, {}},
    {"newline",   {0x5A, R, R}, {}},
    {"enter",     {0x58, R, R}, {}},
    {"reset",     {0x11, R, R}, {}},
    {"quit",      {0x11, R, P}, {}},
    {"setup",     {0x01, R, R}, {}},
};

const struct { const char* alias; const char* name; } kFunctionAliases3270[] = {
    {"attention", "attn"}, {"sysreq", "sysrq"}, {"systemrequest", "sysrq"}, {"eraseinput", "erinp"},
    {"eraseeof", "ereof"}, {"fm", "fieldmark"}, {"duplicate", "dup"},
};

constexpr uint8_t kLowerRow[12] = {0x07, 0x0F, 0x17, 0x1F, 0x27, 0x2F, 0x37, 0x3F, 0x47, 0x4F, 0x56, 0x5E};
constexpr uint8_t kUpperRow[12] = {0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x57, 0x5F};

bool function_key(const std::string& n, std::array<TerminalStroke, 2>& to) {
    // F1..F24: on the 122-key keyboard F13..F24 are the upper row; on the
    // Enhanced keyboard they are Shift+F1..F12.
    if (n.size() >= 2 && n[0] == 'f') {
        uint64_t v = 0;
        if (!parse_uint(n.substr(1), v) || v < 1 || v > 24) return false;
        const size_t i = size_t((v - 1) % 12);
        if (v <= 12) {
            to[0] = {kLowerRow[i], R, R};
            to[1] = {kLowerRow[i], R, R};
        } else {
            to[0] = {kUpperRow[i], R, R};
            to[1] = {kLowerRow[i], P, R};
        }
        return true;
    }
    return false;
}

constexpr uint8_t kTermMods[4] = {0x12, 0x59, 0x19, 0x39};

const char* mod_action_name(ModAction a) {
    switch (a) {
    case ModAction::Keep: return "keep";
    case ModAction::Press: return "press";
    case ModAction::Release: return "release";
    }
    return "?";
}

int term_mod_index(uint8_t code) {
    for (int i = 0; i < 4; ++i)
        if (kTermMods[i] == code) return i;
    return -1;
}

bool cond_matches(ModCond c, uint8_t mods, uint8_t left, uint8_t right) {
    switch (c) {
    case ModCond::Off: return (mods & (left | right)) == 0;
    case ModCond::Either: return (mods & (left | right)) != 0;
    case ModCond::Left: return (mods & left) != 0;
    case ModCond::Right: return (mods & right) != 0;
    case ModCond::Ignore: return true;
    }
    return false;
}

int cond_weight(ModCond c) {
    switch (c) {
    case ModCond::Off: case ModCond::Either: return 1;
    case ModCond::Left: case ModCond::Right: return 2;
    case ModCond::Ignore: return 0;
    }
    return 0;
}

// --- Built-in rules ---------------------------------------------------------

struct BuiltinRule {
    const char* host;
    const char* mods;    // space separated, "" = none, "*" = any
    const char* to;      // function name, "none" or "positional"
    bool tap;
    const char* keyboard;  // "", "122" or "enhanced"
};

// IBM Personal Communications, 5250 default keyboard (IBM default layout for
// the Enhanced keyboard; Personal Communications AS/400 and 3270 for
// Windows 95 V4.1 Reference, SC31-8206, 1.8.1.2, and the installed help;
// details in analysis_notes/pcomm-keymap.md). Printscreen -> Print is ours:
// PCOMM leaves that key to Windows.
const BuiltinRule kPcommRules[] = {
    {"escape", "", "attn", false, ""},
    {"escape", "shift", "sysrq", false, ""},
    {"f1", "alt", "help", false, ""},
    {"pause", "", "clear", false, ""},
    {"pause", "ctrl", "print", false, ""},
    {"pause", "alt", "test", false, ""},
    {"printscreen", "", "print", false, ""},
    {"tab", "shift", "backtab", false, ""},
    {"enter", "", "fieldexit", false, ""},
    {"enter", "shift", "newline", false, ""},
    {"lctrl", "*", "reset", true, ""},
    {"lctrl", "alt", "quit", true, ""},
    {"rctrl", "*", "enter", true, ""},
    {"insert", "*", "insert", false, ""},
    {"insert", "shift", "dup", false, ""},
    {"home", "*", "home", false, ""},
    {"home", "ctrl", "rule", false, ""},
    {"pageup", "*", "rolldown", false, ""},
    {"pageup", "alt", "jump", false, ""},
    {"delete", "*", "delete", false, ""},
    {"end", "*", "none", false, "122"},          // Erase EOF: no such terminal key
    {"end", "*", "positional", false, "enhanced"},
    {"end", "alt", "erinp", false, ""},
    {"end", "ctrl alt", "erinp", false, ""},
    {"end", "altgr", "erinp", false, ""},
    {"pagedown", "*", "rollup", false, ""},
    {"kpminus", "*", "field-", false, ""},
    {"kpplus", "*", "field+", false, ""},
    {"kpenter", "*", "fieldexit", false, ""},
    {"f13", "*", "f13", false, ""}, {"f14", "*", "f14", false, ""}, {"f15", "*", "f15", false, ""},
    {"f16", "*", "f16", false, ""}, {"f17", "*", "f17", false, ""}, {"f18", "*", "f18", false, ""},
    {"f19", "*", "f19", false, ""}, {"f20", "*", "f20", false, ""}, {"f21", "*", "f21", false, ""},
    {"f22", "*", "f22", false, ""}, {"f23", "*", "f23", false, ""}, {"f24", "*", "f24", false, ""},
};

// "mac": the PCOMM map for Mac keyboards, which have no Right Ctrl (PCOMM's
// Enter, the terminal's Enter position). Enter is the most used key, so
// Return sends it, and Field Exit moves to the key Apple labels "enter"
// (keypad Enter; fn+Return on laptop keyboards), which PCOMM already maps
// to Field Exit: the two are swapped against PCOMM. Shift+Return stays New
// Line. Each rule replaces the PCOMM rule with the same host key, modifiers
// and keyboard, in place.
const BuiltinRule kMacOverrides[] = {
    {"enter", "", "enter", false, ""},
};

// "tn5250": the key bindings of the tn5250 terminal emulator (documented:
// tn5250(1), "Keyboard Mapping"), for keyboards without a Right Ctrl or a
// keypad: Return is Enter and the 5250 functions are Ctrl and Alt chords
// (the manual's C- and M- keys). Not carried over: Esc as a Meta prefix,
// Toggle, Refresh and Quit (functions of that program, not terminal keys)
// and + / - as Field Plus / Field Minus inside signed numeric fields (the
// terminal's firmware, not the keymap, knows the field). Ours, marked below:
// Esc and the keypad as in "pcomm", and Shift+Return. The manual's row for
// F1 - F10 reads "M-1 to M-" with the last key missing: Alt+1..9 for F1-F9
// are documented, Alt+0 for F10 is a hypothesis (the next key of the digit
// row, before its Alt+- = F11).
const BuiltinRule kTn5250Rules[] = {
    // Ctrl only selects rules here. Without these the Ctrl keys would press
    // the terminal keys at their positions (Reset and Enter on the 122-key
    // keyboard) at the start of every chord.
    {"lctrl", "*", "none", false, ""},
    {"rctrl", "*", "none", false, ""},
    // The Alt chords are on the Left Alt only [ours]. Right Alt is AltGr on
    // most national keyboards and stays the terminal's Alt, as in "pcomm",
    // so the characters on a key's third legend can still be typed.
    // Return and Delete with any modifier: a rule with no modifiers would
    // leave Ctrl+Return, Shift+Delete and the like to the keys at those
    // positions (Field Exit, New Line). The rules that name a modifier
    // (Shift+Return, Alt+Delete) are more specific and still win.
    {"enter", "*", "enter", false, ""},
    {"kpenter", "*", "enter", false, ""},
    {"j", "ctrl", "enter", false, ""},
    {"m", "ctrl", "enter", false, ""},
    {"j", "ctrl lalt", "newline", false, ""},
    {"enter", "shift", "newline", false, ""},     // ours
    {"tab", "shift", "backtab", false, ""},
    {"i", "ctrl", "tab", false, ""},              // Next Field
    {"pagedown", "*", "rollup", false, ""},
    {"d", "ctrl", "rollup", false, ""},
    {"f", "ctrl", "rollup", false, ""},
    {"pageup", "*", "rolldown", false, ""},
    {"b", "ctrl", "rolldown", false, ""},
    {"u", "ctrl", "rolldown", false, ""},
    {"home", "*", "home", false, ""},
    {"o", "ctrl", "home", false, ""},
    {"end", "*", "none", false, "122"},           // End of field: no such terminal key
    {"end", "*", "positional", false, "enhanced"},
    {"insert", "*", "insert", false, ""},
    {"i", "lalt", "insert", false, ""},
    {"delete", "lalt", "insert", false, ""},
    {"delete", "*", "delete", false, ""},
    {"r", "ctrl", "reset", false, ""},
    {"r", "lalt", "reset", false, ""},
    {"p", "ctrl", "print", false, ""},
    {"p", "lalt", "print", false, ""},
    {"h", "lalt", "help", false, ""},
    {"c", "ctrl", "sysrq", false, ""},
    {"s", "lalt", "sysrq", false, ""},
    {"c", "lalt", "clear", false, ""},
    {"k", "ctrl", "fieldexit", false, ""},
    {"x", "lalt", "fieldexit", false, ""},
    {"t", "ctrl", "test", false, ""},
    {"e", "ctrl", "erinp", false, ""},
    {"a", "ctrl", "attn", false, ""},
    {"a", "lalt", "attn", false, ""},
    {"d", "lalt", "dup", false, ""},
    {"x", "ctrl", "field+", false, ""},
    {"m", "lalt", "field-", false, ""},
    {"escape", "", "attn", false, ""},            // ours (pcomm)
    {"escape", "shift", "sysrq", false, ""},      // ours (pcomm)
    {"kpplus", "*", "field+", false, ""},         // ours (pcomm)
    {"kpminus", "*", "field-", false, ""},        // ours (pcomm)
    {"1", "lalt", "f1", false, ""}, {"2", "lalt", "f2", false, ""}, {"3", "lalt", "f3", false, ""},
    {"4", "lalt", "f4", false, ""}, {"5", "lalt", "f5", false, ""}, {"6", "lalt", "f6", false, ""},
    {"7", "lalt", "f7", false, ""}, {"8", "lalt", "f8", false, ""}, {"9", "lalt", "f9", false, ""},
    {"0", "lalt", "f10", false, ""},               // hypothesis
    {"minus", "lalt", "f11", false, ""}, {"equals", "lalt", "f12", false, ""},
    // M-! to M-+: Alt with the shifted digit row.
    {"1", "lalt shift", "f13", false, ""}, {"2", "lalt shift", "f14", false, ""},
    {"3", "lalt shift", "f15", false, ""}, {"4", "lalt shift", "f16", false, ""},
    {"5", "lalt shift", "f17", false, ""}, {"6", "lalt shift", "f18", false, ""},
    {"7", "lalt shift", "f19", false, ""}, {"8", "lalt shift", "f20", false, ""},
    {"9", "lalt shift", "f21", false, ""}, {"0", "lalt shift", "f22", false, ""},
    {"minus", "lalt shift", "f23", false, ""}, {"equals", "lalt shift", "f24", false, ""},
    {"f13", "*", "f13", false, ""}, {"f14", "*", "f14", false, ""}, {"f15", "*", "f15", false, ""},
    {"f16", "*", "f16", false, ""}, {"f17", "*", "f17", false, ""}, {"f18", "*", "f18", false, ""},
    {"f19", "*", "f19", false, ""}, {"f20", "*", "f20", false, ""}, {"f21", "*", "f21", false, ""},
    {"f22", "*", "f22", false, ""}, {"f23", "*", "f23", false, ""}, {"f24", "*", "f24", false, ""},
};

// "pcomm3270": IBM Personal Communications' 3270 default keyboard for the
// Enhanced keyboard [documented: S20H-1773, 5.1.2, "PS/2 Enhanced
// Keyboard": Esc Attn (Shift: SysReq), Pause Clear, Insert (Shift: Dup,
// Alt: PA1), Home (Shift: Field Mark, Ctrl: Rule, Alt: PA2), Page Up with
// Shift PA3, End Erase EOF (Alt: Erase Input), Return New Line, Left Ctrl
// Reset, Right Ctrl and keypad Enter Enter], in the 3270 legends of the
// coax terminals' 122-key keyboard. Page Up and Page Down alone do nothing
// there.
const BuiltinRule kPcomm3270Rules[] = {
    {"escape", "", "3270:attn", false, ""},
    {"escape", "shift", "3270:sysrq", false, ""},
    {"pause", "", "3270:clear", false, ""},
    {"tab", "shift", "3270:backtab", false, ""},
    {"enter", "", "3270:newline", false, ""},
    {"enter", "shift", "3270:newline", false, ""},
    {"lctrl", "*", "3270:reset", true, ""},
    {"lctrl", "alt", "3270:quit", true, ""},
    {"rctrl", "*", "3270:enter", true, ""},
    {"kpenter", "*", "3270:enter", false, ""},
    {"insert", "*", "3270:insert", false, ""},
    {"insert", "shift", "3270:dup", false, ""},
    {"insert", "alt", "3270:pa1", false, ""},
    {"home", "*", "3270:home", false, ""},
    {"home", "shift", "3270:fieldmark", false, ""},
    {"home", "ctrl", "3270:rule", false, ""},
    {"home", "alt", "3270:pa2", false, ""},
    {"pageup", "*", "none", false, ""},
    {"pageup", "shift", "3270:pa3", false, ""},
    {"pagedown", "*", "none", false, ""},
    {"delete", "*", "3270:delete", false, ""},
    {"end", "*", "3270:ereof", false, ""},
    {"end", "alt", "3270:erinp", false, ""},
    {"f13", "*", "f13", false, ""}, {"f14", "*", "f14", false, ""}, {"f15", "*", "f15", false, ""},
    {"f16", "*", "f16", false, ""}, {"f17", "*", "f17", false, ""}, {"f18", "*", "f18", false, ""},
    {"f19", "*", "f19", false, ""}, {"f20", "*", "f20", false, ""}, {"f21", "*", "f21", false, ""},
    {"f22", "*", "f22", false, ""}, {"f23", "*", "f23", false, ""}, {"f24", "*", "f24", false, ""},
};

// "mac3270": "pcomm3270" for Mac keyboards, which have no Right Ctrl: Return
// is Enter and keypad Enter (fn+Return) New Line, as "mac" does for 5250.
const BuiltinRule kMac3270Overrides[] = {
    {"enter", "", "3270:enter", false, ""},
    {"kpenter", "*", "3270:newline", false, ""},
};

// The rules of a built-in map with rules ("pcomm", "mac" or "tn5250"), in
// order.
std::vector<BuiltinRule> builtin_rules(const std::string& normalised) {
    if (normalised == "tn5250") return std::vector<BuiltinRule>(std::begin(kTn5250Rules), std::end(kTn5250Rules));
    if (normalised == "pcomm3270" || normalised == "mac3270") {
        std::vector<BuiltinRule> rules(std::begin(kPcomm3270Rules), std::end(kPcomm3270Rules));
        if (normalised == "mac3270")
            for (BuiltinRule& r : rules)
                for (const BuiltinRule& o : kMac3270Overrides)
                    if (std::string(r.host) == o.host && std::string(r.mods) == o.mods) r = o;
        return rules;
    }
    std::vector<BuiltinRule> rules(std::begin(kPcommRules), std::end(kPcommRules));
    if (normalised == "mac") {
        for (BuiltinRule& r : rules)
            for (const BuiltinRule& o : kMacOverrides)
                if (std::string(r.host) == o.host && std::string(r.mods) == o.mods &&
                    std::string(r.keyboard) == o.keyboard)
                    r = o;
    }
    return rules;
}

bool parse_mod_token(const std::string& tok, KeymapRule& r) {
    const std::string t = normalise(tok);
    if (t.empty()) return true;
    if (t == "*" || t == "any") {
        if (r.shift == ModCond::Off) r.shift = ModCond::Ignore;
        if (r.ctrl == ModCond::Off) r.ctrl = ModCond::Ignore;
        if (r.alt == ModCond::Off) r.alt = ModCond::Ignore;
        return true;
    }
    if (t == "shift") r.shift = ModCond::Either;
    else if (t == "lshift" || t == "leftshift") r.shift = ModCond::Left;
    else if (t == "rshift" || t == "rightshift") r.shift = ModCond::Right;
    else if (t == "ctrl" || t == "control") r.ctrl = ModCond::Either;
    else if (t == "lctrl" || t == "leftctrl") r.ctrl = ModCond::Left;
    else if (t == "rctrl" || t == "rightctrl") r.ctrl = ModCond::Right;
    else if (t == "alt") r.alt = ModCond::Either;
    else if (t == "lalt" || t == "leftalt") r.alt = ModCond::Left;
    else if (t == "ralt" || t == "rightalt" || t == "altgr") r.alt = ModCond::Right;
    else return false;
    return true;
}

// Explicit conditions set by tokens override "*" whatever their order.
bool parse_mods(const std::vector<std::string>& toks, KeymapRule& r, std::string& bad) {
    r.shift = r.ctrl = r.alt = ModCond::Off;
    bool any = false;
    for (const std::string& t : toks) {
        const std::string n = normalise(t);
        if (n == "*" || n == "any") { any = true; continue; }
        if (!parse_mod_token(t, r)) { bad = t; return false; }
    }
    if (any) {
        if (r.shift == ModCond::Off) r.shift = ModCond::Ignore;
        if (r.ctrl == ModCond::Off) r.ctrl = ModCond::Ignore;
        if (r.alt == ModCond::Off) r.alt = ModCond::Ignore;
    }
    return true;
}

bool parse_keyboard_filter(const std::string& s, std::array<bool, 2>& applies) {
    const std::string n = normalise(s);
    if (n.empty() || n == "both" || n == "all") { applies = {{true, true}}; return true; }
    KeyboardType t;
    if (!parse_keyboard_type(n, t)) return false;
    applies = {{t == KeyboardType::Terminal122, t == KeyboardType::Enhanced}};
    return true;
}

// Fills rule.to / positional from a target name.
bool set_target(const std::string& name, KeymapRule& r) {
    const std::string n = normalise(name);
    if (n == "none") {
        r.to = {};
        return true;
    }
    if (n == "positional" || n == "passthrough") {
        r.positional = true;
        return true;
    }
    std::array<bool, 2> ok{};
    if (!terminal_target_from_name(name, r.to, ok)) return false;
    for (int i = 0; i < 2; ++i)
        if (!ok[size_t(i)]) r.applies[size_t(i)] = false;
    return true;
}

void add_shift_fkey_rules(Keymap& km) {
    for (int i = 0; i < 12; ++i) {
        KeymapRule r;
        r.hid = 0x3A + uint32_t(i);
        r.shift = ModCond::Either;
        r.applies = {{true, false}};
        r.to[0] = {kUpperRow[i], R, K};
        r.source = "positional";
        km.add(r);
    }
}

bool parse_mod_action(const std::string& s, ModAction& out) {
    const std::string n = normalise(s);
    if (n == "keep") out = K;
    else if (n == "press" || n == "hold" || n == "on") out = P;
    else if (n == "release" || n == "off") out = R;
    else return false;
    return true;
}

} // namespace

// --- Keymap ----------------------------------------------------------------

int KeymapRule::specificity() const { return cond_weight(shift) + cond_weight(ctrl) + cond_weight(alt); }

const KeymapRule* Keymap::find(KeyboardType type, uint32_t hid, uint8_t mods) const {
    const int ti = type_index(type);
    const KeymapRule* best = nullptr;
    int best_score = -1;
    for (const KeymapRule& r : rules_) {
        if (r.hid != hid || !r.applies[size_t(ti)]) continue;
        if (!cond_matches(r.shift, mods, kModLShift, kModRShift)) continue;
        if (!cond_matches(r.ctrl, mods, kModLCtrl, kModRCtrl)) continue;
        if (!cond_matches(r.alt, mods, kModLAlt, kModRAlt)) continue;
        const int s = r.specificity();
        if (s >= best_score) {
            best = &r;
            best_score = s;
        }
    }
    return best;
}

uint8_t host_modifier_bit(uint32_t hid) {
    switch (hid) {
    case 0xE0: return kModLCtrl;
    case 0xE1: return kModLShift;
    case 0xE2: return kModLAlt;
    case 0xE4: return kModRCtrl;
    case 0xE5: return kModRShift;
    case 0xE6: return kModRAlt;
    default: return 0;
    }
}

bool host_key_from_name(const std::string& name, uint32_t& hid) {
    std::string n = normalise(name);
    if (starts_with(n, "keypad")) n = "kp" + n.substr(6);
    if (starts_with(n, "hid:")) n = n.substr(4);
    if (starts_with(n, "0x")) {
        uint64_t v;
        if (!parse_uint(n.substr(2), v, 16) || v > 0xFFFF) return false;
        hid = uint32_t(v);
        return true;
    }
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') { hid = 0x04 + uint32_t(n[0] - 'a'); return true; }
    if (n.size() == 1 && n[0] >= '1' && n[0] <= '9') { hid = 0x1E + uint32_t(n[0] - '1'); return true; }
    if (n == "0") { hid = 0x27; return true; }
    if (n.size() == 3 && starts_with(n, "kp") && n[2] >= '1' && n[2] <= '9') {
        hid = 0x59 + uint32_t(n[2] - '1');
        return true;
    }
    if (n == "kp0") { hid = 0x62; return true; }
    if (n.size() >= 2 && n[0] == 'f') {
        uint64_t v;
        if (parse_uint(n.substr(1), v) && v >= 1 && v <= 24) {
            hid = v <= 12 ? 0x3A + uint32_t(v - 1) : 0x68 + uint32_t(v - 13);
            return true;
        }
    }
    for (const HostName& h : kHostNames)
        if (n == h.name) { hid = h.hid; return true; }
    return false;
}

bool terminal_target_from_name(const std::string& name, std::array<TerminalStroke, 2>& to,
                               std::array<bool, 2>& ok) {
    // "field+" / "field-": a trailing sign is part of the name.
    std::string raw = to_lower(trim(name));
    if (!raw.empty() && raw.back() == '+') raw = raw.substr(0, raw.size() - 1) + "plus";
    else if (!raw.empty() && raw.back() == '-') raw = raw.substr(0, raw.size() - 1) + "minus";
    std::string n = normalise(raw);
    ok = {{true, true}};
    if (starts_with(n, "set3:")) n = "0x" + n.substr(5);
    if (starts_with(n, "0x")) {
        uint64_t v;
        if (!parse_uint(n.substr(2), v, 16) || v == 0 || v > 0xEC) return false;
        to[0] = to[1] = {uint8_t(v), K, K};
        return true;
    }
    if (starts_with(n, "3270:")) {
        n = n.substr(5);
        for (const auto& a : kFunctionAliases3270)
            if (n == a.alias) n = a.name;
        if (function_key(n, to)) {
            ok = {{true, false}};
            return true;
        }
        for (const Fn& f : kFunctions3270)
            if (n == f.name) {
                to[0] = f.t122;
                to[1] = {};
                ok = {{true, false}};
                return true;
            }
        return false;
    }
    for (const auto& a : kFunctionAliases)
        if (n == a.alias) n = a.name;
    if (function_key(n, to)) return true;
    for (const Fn& f : kFunctions) {
        if (n == f.name) {
            to[0] = f.t122;
            to[1] = f.tenh;
            ok = {{f.t122.code != 0, f.tenh.code != 0}};
            return true;
        }
    }
    return false;
}

std::vector<std::string> terminal_function_names() {
    std::vector<std::string> out;
    for (const Fn& f : kFunctions) out.push_back(f.name);
    for (int i = 1; i <= 24; ++i) out.push_back(format("f%d", i));
    return out;
}

bool builtin_keymap(const std::string& name, bool shift_fkeys, Keymap& out) {
    const std::string n = normalise(name);
    if (n != "positional" && n != "pcomm" && n != "mac" && n != "tn5250" && n != "pcomm3270" && n != "mac3270" &&
        n != "none")
        return false;
    out = Keymap();
    if (n == "none") return true;
    if (shift_fkeys) add_shift_fkey_rules(out);
    if (n == "positional") return true;
    for (const BuiltinRule& b : builtin_rules(n)) {
        KeymapRule r;
        std::string bad;
        if (!host_key_from_name(b.host, r.hid) || !parse_mods(split_words(b.mods), r, bad) ||
            !parse_keyboard_filter(b.keyboard, r.applies) || !set_target(b.to, r))
            return false;   // table error; caught by the unit tests
        r.tap = b.tap;
        r.source = n;
        out.add(r);
    }
    return true;
}

std::string keymap_for_3270(const std::string& spec) {
    const std::string n = normalise(spec);
    if (n == "pcomm" || n == "tn5250") return "pcomm3270";
    if (n == "mac") return "mac3270";
    return spec;
}

bool parse_keymap(const std::string& text, const std::string& origin, bool shift_fkeys, Keymap& out,
                  std::string& error) {
    toml::table root;
    try {
        root = toml::parse(text, origin);
    } catch (const toml::parse_error& e) {
        std::ostringstream os;
        os << origin << ": " << e.description() << " (line " << e.source().begin.line << ")";
        error = os.str();
        return false;
    }
    std::string base = "pcomm";
    for (const auto& kv : root) {
        const std::string key(kv.first.str());
        if (key == "base") {
            if (auto v = kv.second.value<std::string>()) base = *v;
            else { error = origin + ": base must be a string"; return false; }
        } else if (key != "map") {
            error = origin + ": unknown key '" + key + "' (expected base and [[map]] entries)";
            return false;
        }
    }
    if (!builtin_keymap(base, shift_fkeys, out)) {
        error = origin + ": base must be \"pcomm\", \"mac\", \"tn5250\", \"positional\" or \"none\"";
        return false;
    }
    const toml::node* maps = root.get("map");
    if (!maps) return true;
    const toml::array* arr = maps->as_array();
    if (!arr) { error = origin + ": map must be an array of tables ([[map]])"; return false; }
    for (const toml::node& node : *arr) {
        const toml::table* t = node.as_table();
        const std::string where = origin + ":" + std::to_string(node.source().begin.line);
        if (!t) { error = where + ": [[map]] entries must be tables"; return false; }
        KeymapRule r;
        r.source = where;
        bool have_host = false, have_to = false;
        std::string shift_s, alt_s;
        for (const auto& kv : *t) {
            const std::string key(kv.first.str());
            const toml::node& v = kv.second;
            if (key == "host") {
                if (auto s = v.value<std::string>()) {
                    if (!host_key_from_name(*s, r.hid)) { error = where + ": unknown host key '" + *s + "'"; return false; }
                } else if (auto i = v.value<int64_t>()) {
                    if (*i <= 0 || *i > 0xFFFF) { error = where + ": host usage out of range"; return false; }
                    r.hid = uint32_t(*i);
                } else {
                    error = where + ": host must be a key name or HID usage";
                    return false;
                }
                have_host = true;
            } else if (key == "mods") {
                std::vector<std::string> toks;
                if (auto s = v.value<std::string>()) toks = split_words(*s);
                else if (const toml::array* a = v.as_array()) {
                    for (const toml::node& m : *a) {
                        auto s2 = m.value<std::string>();
                        if (!s2) { error = where + ": mods must be strings"; return false; }
                        toks.push_back(*s2);
                    }
                } else {
                    error = where + ": mods must be a string or an array of strings";
                    return false;
                }
                std::string bad;
                if (!parse_mods(toks, r, bad)) { error = where + ": unknown modifier '" + bad + "'"; return false; }
            } else if (key == "to") {
                if (auto s = v.value<std::string>()) {
                    if (!set_target(*s, r)) { error = where + ": unknown terminal key '" + *s + "'"; return false; }
                } else if (auto i = v.value<int64_t>()) {
                    if (*i <= 0 || *i > 0xEC) { error = where + ": set-3 code out of range"; return false; }
                    r.to[0] = r.to[1] = {uint8_t(*i), K, K};
                } else {
                    error = where + ": to must be a terminal key name or set-3 code";
                    return false;
                }
                have_to = true;
            } else if (key == "keyboard") {
                auto s = v.value<std::string>();
                std::array<bool, 2> f{};
                if (!s || !parse_keyboard_filter(*s, f)) {
                    error = where + ": keyboard must be \"122\", \"enhanced\" or \"both\"";
                    return false;
                }
                for (size_t i = 0; i < 2; ++i) r.applies[i] = r.applies[i] && f[i];
            } else if (key == "shift" || key == "alt") {
                auto s = v.value<std::string>();
                if (!s) { error = where + ": " + key + " must be keep, press or release"; return false; }
                (key == "shift" ? shift_s : alt_s) = *s;
            } else if (key == "tap") {
                auto b = v.value<bool>();
                if (!b) { error = where + ": tap must be true or false"; return false; }
                r.tap = *b;
            } else {
                error = where + ": unknown key '" + key + "' in [[map]]";
                return false;
            }
        }
        if (!have_host || !have_to) { error = where + ": [[map]] needs host and to"; return false; }
        for (int i = 0; i < 2; ++i) {
            ModAction a;
            if (!shift_s.empty()) {
                if (!parse_mod_action(shift_s, a)) { error = where + ": shift must be keep, press or release"; return false; }
                r.to[size_t(i)].shift = a;
            }
            if (!alt_s.empty()) {
                if (!parse_mod_action(alt_s, a)) { error = where + ": alt must be keep, press or release"; return false; }
                r.to[size_t(i)].alt = a;
            }
        }
        out.add(r);
    }
    return true;
}

bool load_keymap(const std::string& spec, bool shift_fkeys, Keymap& out, std::string& error) {
    if (spec.empty() || builtin_keymap(spec, shift_fkeys, out)) {
        if (spec.empty()) builtin_keymap("pcomm", shift_fkeys, out);
        return true;
    }
    std::ifstream f(spec, std::ios::binary);
    if (!f) {
        error = "keymap '" + spec + "' is neither pcomm, mac, tn5250, positional nor a readable file";
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_keymap(ss.str(), spec, shift_fkeys, out, error);
}

// --- Keymap files as text -----------------------------------------------------

namespace {

std::string node_text(const toml::node& v) {
    if (v.is_boolean()) return *v.value<bool>() ? "true" : "false";
    if (v.is_integer()) return format("0x%02llX", static_cast<unsigned long long>(*v.value<int64_t>()));
    if (v.is_string()) return *v.value<std::string>();
    if (const toml::array* a = v.as_array()) {
        std::string out;
        for (const toml::node& m : *a)
            if (auto s = m.value<std::string>()) out += (out.empty() ? "" : " ") + *s;
        return out;
    }
    return std::string();
}

struct PrettyName {
    uint32_t hid;
    const char* name;
};

const PrettyName kPrettyNames[] = {
    {0x28, "Enter"}, {0x29, "Escape"}, {0x2A, "Backspace"}, {0x2B, "Tab"}, {0x2C, "Space"},
    {0x2D, "Minus"}, {0x2E, "Equals"}, {0x2F, "LeftBracket"}, {0x30, "RightBracket"}, {0x31, "Backslash"},
    {0x32, "NonUSHash"}, {0x33, "Semicolon"}, {0x34, "Apostrophe"}, {0x35, "Grave"}, {0x36, "Comma"},
    {0x37, "Period"}, {0x38, "Slash"}, {0x39, "CapsLock"}, {0x46, "PrintScreen"}, {0x47, "ScrollLock"},
    {0x48, "Pause"}, {0x49, "Insert"}, {0x4A, "Home"}, {0x4B, "PageUp"}, {0x4C, "Delete"}, {0x4D, "End"},
    {0x4E, "PageDown"}, {0x4F, "Right"}, {0x50, "Left"}, {0x51, "Down"}, {0x52, "Up"}, {0x53, "NumLock"},
    {0x54, "KP_Divide"}, {0x55, "KP_Multiply"}, {0x56, "KP_Minus"}, {0x57, "KP_Plus"}, {0x58, "KP_Enter"},
    {0x63, "KP_Period"}, {0x64, "NonUSBackslash"}, {0x65, "Application"}, {0x67, "KP_Equals"},
    {0xE0, "LCtrl"}, {0xE1, "LShift"}, {0xE2, "LAlt"}, {0xE3, "LGui"}, {0xE4, "RCtrl"}, {0xE5, "RShift"},
    {0xE6, "RAlt"}, {0xE7, "RGui"},
};

} // namespace

std::string host_key_name(uint32_t hid) {
    if (hid >= 0x04 && hid <= 0x1D) return std::string(1, char('a' + (hid - 0x04)));
    if (hid >= 0x1E && hid <= 0x26) return std::string(1, char('1' + (hid - 0x1E)));
    if (hid == 0x27) return "0";
    if (hid >= 0x3A && hid <= 0x45) return format("F%u", hid - 0x3A + 1);
    if (hid >= 0x68 && hid <= 0x73) return format("F%u", hid - 0x68 + 13);
    if (hid >= 0x59 && hid <= 0x61) return format("KP_%u", hid - 0x59 + 1);
    if (hid == 0x62) return "KP_0";
    for (const PrettyName& p : kPrettyNames)
        if (p.hid == hid) return p.name;
    return format("0x%02X", hid);
}

std::string host_mods_text(uint8_t mods) {
    std::string out;
    auto add = [&](uint8_t l, uint8_t r, const char* both, const char* left, const char* right) {
        const bool hl = (mods & l) != 0, hr = (mods & r) != 0;
        if (!hl && !hr) return;
        if (!out.empty()) out += ' ';
        out += hl && hr ? both : hl ? left : right;
    };
    add(kModLShift, kModRShift, "shift", "shift", "shift");
    add(kModLCtrl, kModRCtrl, "ctrl", "ctrl", "ctrl");
    add(kModLAlt, kModRAlt, "alt", "alt", "altgr");
    return out;
}

bool parse_keymap_file(const std::string& text, const std::string& origin, KeymapFile& out, std::string& error) {
    Keymap check;
    if (!parse_keymap(text, origin, true, check, error)) return false;
    toml::table root = toml::parse(text, origin);   // parse_keymap accepted it
    out = KeymapFile();
    if (auto b = root["base"].value<std::string>()) out.base = *b;
    if (const toml::array* arr = root["map"].as_array()) {
        for (const toml::node& node : *arr) {
            const toml::table* t = node.as_table();
            if (!t) continue;
            KeymapEntry e;
            for (const auto& kv : *t) {
                const std::string key(kv.first.str());
                const std::string v = node_text(kv.second);
                if (key == "host") {
                    uint32_t hid = 0;
                    e.host = kv.second.is_integer() && host_key_from_name(v, hid) ? host_key_name(hid) : v;
                } else if (key == "mods") e.mods = v;
                else if (key == "to") e.to = v;
                else if (key == "keyboard") e.keyboard = v;
                else if (key == "shift") e.shift = v;
                else if (key == "alt") e.alt = v;
                else if (key == "tap") e.tap = v == "true";
            }
            out.entries.push_back(e);
        }
    }
    return true;
}

namespace {
// TOML basic string with the few escapes keymap values can need.
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}
} // namespace

std::string keymap_file_to_toml(const KeymapFile& file, const std::string& comment) {
    std::string out;
    if (!comment.empty()) {
        for (const std::string& line : split(comment, '\n')) out += (line.empty() ? "#" : "# " + line) + "\n";
        out += "\n";
    }
    out += "base = " + quoted(file.base) + "\n";
    for (const KeymapEntry& e : file.entries) {
        out += "\n[[map]]\nhost = " + quoted(e.host) + "\n";
        if (!e.mods.empty()) out += "mods = " + quoted(e.mods) + "\n";
        out += "to = " + quoted(e.to) + "\n";
        if (!e.keyboard.empty()) out += "keyboard = " + quoted(e.keyboard) + "\n";
        if (!e.shift.empty()) out += "shift = " + quoted(e.shift) + "\n";
        if (!e.alt.empty()) out += "alt = " + quoted(e.alt) + "\n";
        if (e.tap) out += "tap = true\n";
    }
    return out;
}

bool check_keymap_entry(const KeymapEntry& e, std::string& error) {
    KeymapFile f;
    f.base = "none";
    f.entries.push_back(e);
    Keymap km;
    return parse_keymap(keymap_file_to_toml(f), "mapping", true, km, error);
}

namespace {
KeymapFile builtin_keymap_file(const std::string& name) {
    KeymapFile f;
    f.base = "positional";
    for (const BuiltinRule& b : builtin_rules(name)) {
        KeymapEntry e;
        uint32_t hid = 0;
        e.host = host_key_from_name(b.host, hid) ? host_key_name(hid) : b.host;
        e.mods = b.mods;
        e.to = b.to;
        e.keyboard = b.keyboard;
        e.tap = b.tap;
        f.entries.push_back(e);
    }
    return f;
}
} // namespace

KeymapFile pcomm_keymap_file() { return builtin_keymap_file("pcomm"); }
KeymapFile mac_keymap_file() { return builtin_keymap_file("mac"); }
KeymapFile tn5250_keymap_file() { return builtin_keymap_file("tn5250"); }

std::string keymap_base_text(const std::string& base) {
    if (base == "positional")
        return "Keys not listed press the terminal key at the same position; Shift+F1..F12 enter F13..F24 on the "
               "122-key keyboard.";
    if (base == "pcomm") return "Keys not listed follow the built-in PCOMM layout (base \"pcomm\").";
    if (base == "mac") return "Keys not listed follow the built-in Mac layout (base \"mac\").";
    if (base == "tn5250") return "Keys not listed follow the built-in tn5250 layout (base \"tn5250\").";
    return "Keys not listed press the terminal key at the same position.";
}

// --- Engine ------------------------------------------------------------------

void HostKeymapEngine::reset() {
    down_.clear();
    host_mods_ = 0;
    refs_.fill(0);
    phys_.fill(0);
    sent_.fill(false);
    shift_off_ = shift_on_ = alt_off_ = alt_on_ = 0;
}

void HostKeymapEngine::sync_mods(const Emit& emit) {
    // Releases first, then presses.
    for (const bool pass : {false, true}) {
        for (int i = 0; i < 4; ++i) {
            const bool is_shift = i < 2;
            const bool off = is_shift ? shift_off_ > 0 : alt_off_ > 0;
            const bool on = (i == 0 && shift_on_ > 0) || (i == 2 && alt_on_ > 0);
            const bool want = !off && (phys_[size_t(i)] > 0 || on);
            if (want != sent_[size_t(i)] && want == pass) {
                sent_[size_t(i)] = want;
                emit(kTermMods[i], want);
            }
        }
    }
}

void HostKeymapEngine::press_stroke(const TerminalStroke& s, bool pass_mod, const Emit& emit) {
    if (pass_mod) {
        ++phys_[size_t(term_mod_index(s.code))];
        sync_mods(emit);
        return;
    }
    if (s.shift == R) ++shift_off_;
    if (s.shift == P) ++shift_on_;
    if (s.alt == R) ++alt_off_;
    if (s.alt == P) ++alt_on_;
    sync_mods(emit);
    if (refs_[s.code]++ == 0) emit(s.code, true);
}

void HostKeymapEngine::release_stroke(const TerminalStroke& s, bool pass_mod, const Emit& emit) {
    if (pass_mod) {
        uint8_t& n = phys_[size_t(term_mod_index(s.code))];
        if (n) --n;
        sync_mods(emit);
        return;
    }
    if (refs_[s.code] && --refs_[s.code] == 0) emit(s.code, false);
    if (s.shift == R) --shift_off_;
    if (s.shift == P) --shift_on_;
    if (s.alt == R) --alt_off_;
    if (s.alt == P) --alt_on_;
    sync_mods(emit);
}

bool HostKeymapEngine::host_key(KeyboardType type, uint32_t hid, bool pressed, const Emit& emit) {
    const uint8_t bit = host_modifier_bit(hid);
    if (!pressed) {
        host_mods_ = uint8_t(host_mods_ & ~bit);
        auto it = down_.find(hid);
        if (it == down_.end()) return true;
        const Down d = it->second;
        down_.erase(it);
        if (d.stroke.code == 0) return true;
        if (d.tap) {
            TN_LOG("input", Debug, "keymap: HID %02X up, tap %s", hid,
                   d.consumed ? "cancelled (another key was pressed meanwhile)" : "-> sending set-3 key");
            if (!d.consumed) {
                press_stroke(d.stroke, false, emit);
                release_stroke(d.stroke, false, emit);
            }
        } else {
            release_stroke(d.stroke, d.pass_mod, emit);
        }
        return true;
    }

    if (down_.count(hid)) {
        TN_LOG("input", Debug, "keymap: HID %02X down again without a release (host repeat), ignored", hid);
        return true;   // host auto-repeat; the keyboard repeats itself
    }
    for (auto& kv : down_) {
        if (kv.second.tap && !kv.second.consumed)
            TN_LOG("input", Debug, "keymap: HID %02X cancels the tap of HID %02X (used as a modifier)", hid,
                   kv.first);
        kv.second.consumed = true;
    }
    const uint8_t mods = host_mods_;     // the key's own modifier bit is not part of the match
    host_mods_ = uint8_t(host_mods_ | bit);

    Down d;
    const KeymapRule* rule = keymap_ ? keymap_->find(type, hid, mods) : nullptr;
    if (rule && !rule->positional) {
        d.stroke = rule->to[size_t(type_index(type))];
        d.tap = rule->tap;
    } else {
        d.stroke.code = positional_ ? positional_(type, hid) : 0;
        d.tap = rule && rule->tap;
    }
    d.pass_mod = !d.tap && term_mod_index(d.stroke.code) >= 0;
    TN_LOG("input", Debug, "keymap: HID %02X down, host mods %02X -> %s%s -> set-3 %02X shift %s alt %s%s", hid,
           mods, rule ? rule->source.c_str() : "no rule", rule && rule->positional ? " (positional)" : "",
           d.stroke.code, mod_action_name(d.stroke.shift), mod_action_name(d.stroke.alt),
           d.tap ? ", tap: sent on release if used alone" : "");
    down_[hid] = d;
    if (d.stroke.code == 0) return rule != nullptr;
    if (!d.tap) press_stroke(d.stroke, d.pass_mod, emit);
    return true;
}

} // namespace em34xx
