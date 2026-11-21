// SPDX-License-Identifier: GPL-3.0-or-later
#include "input/keyboard_ids.hpp"

#include "machine/machine.hpp"
#include "util/strings.hpp"

namespace em34xx {

const std::vector<KeyboardLayoutInfo>& keyboard_layouts() {
    // Source: IBM InfoWindow II 3489 Maintenance Information, SY18-2345-0,
    // 1.2.1.1 "Keyboard IDs". Every pair obeys id122 == id_enhanced ^ 0xC0;
    // the one blank cell (Dutch 122-key) is filled from that rule.
    static const std::vector<KeyboardLayoutInfo> layouts = {
        {"us",           "U.S. English",                           0xBF, 0x7F, true},
        {"us-dataentry", "Data Entry - U.S. English",              0x9F, 0x00, true},
        {"ca",           "Canadian French",                        0xBB, 0x7B, true},
        {"latam",        "Latin American Spanish",                 0xB0, 0x70, true},
        {"jp",           "Japanese Katakana",                      0xB4, 0x74, true},
        {"de",           "Austrian / German",                      0xBE, 0x7E, true},
        {"be",           "Belgian",                                0xBD, 0x7D, true},
        {"dk",           "Danish",                                 0xBA, 0x7A, true},
        {"se",           "Finnish / Swedish",                      0xAF, 0x6F, true},
        {"fr",           "French AZERTY",                          0xB9, 0x79, true},
        {"it",           "Italian",                                0xB6, 0x76, true},
        {"nl",           "Dutch (122-key ID inferred)",            0xAB, 0x6B, false},
        {"no",           "Norwegian",                              0xB3, 0x73, true},
        {"pt",           "Portuguese",                             0xB2, 0x72, true},
        {"es",           "Spanish",                                0xB1, 0x71, true},
        {"ch-fr",        "Swiss-French",                           0xAD, 0x6D, true},
        {"ch-de",        "Swiss-German",                           0xAE, 0x6E, true},
        {"uk",           "U.K. English",                           0xAC, 0x6C, true},
        {"ar",           "Arabic",                                 0xB7, 0x77, true},
        {"he",           "Hebrew",                                 0xA7, 0x67, true},
        {"gr",           "Greek",                                  0xA8, 0x68, true},
        {"is",           "Icelandic",                              0xA6, 0x66, true},
        {"tr",           "Turkish",                                0xA3, 0x63, true},
        {"hr",           "Croatian / Slovenian / Serbian Latin",   0xA2, 0x62, true},
        {"hu",           "Hungarian",                              0x00, 0x41, true},
        {"cz",           "Czech",                                  0x00, 0x42, true},
        {"sk",           "Slovak",                                 0x00, 0x43, true},
        {"pl",           "Polish",                                 0x00, 0x44, true},
        {"ro",           "Romanian",                               0x00, 0x45, true},
        {"ru",           "Russian Cyrillic",                       0x00, 0x46, true},
        {"mk",           "Macedonian / Serbian Cyrillic",          0x00, 0x47, true},
        {"tr-new",       "New Turkish",                            0x88, 0x48, true},
        {"bg",           "Bulgarian Cyrillic",                     0x00, 0x49, true},
        {"gr-new",       "New Greek",                              0xA4, 0x64, true},
        {"latam-new",    "New Latin American Spanish",             0xAA, 0x00, true},
        {"br",           "Brazilian Portuguese",                   0x00, 0x78, true},
        {"th",           "Thai",                                   0x00, 0x75, true},
    };
    return layouts;
}

const KeyboardLayoutInfo* find_keyboard_layout(const std::string& name) {
    static const struct { const char* alias; const char* name; } aliases[] = {
        {"en", "us"}, {"gb", "uk"}, {"at", "de"}, {"fi", "se"}, {"sv", "se"},
        {"da", "dk"}, {"cs", "cz"}, {"el", "gr"}, {"iw", "he"}, {"si", "hr"},
        {"sl", "hr"}, {"pt-br", "br"}, {"ja", "jp"},
    };
    std::string n = to_lower(trim(name));
    for (const auto& a : aliases)
        if (n == a.alias) n = a.name;
    for (const KeyboardLayoutInfo& l : keyboard_layouts())
        if (n == l.name) return &l;
    return nullptr;
}

bool parse_keyboard_type(const std::string& text, KeyboardType& out) {
    const std::string t = to_lower(trim(text));
    if (t == "122" || t == "124" || t == "terminal" || t == "122-key") {
        out = KeyboardType::Terminal122;
        return true;
    }
    if (t == "enhanced" || t == "101" || t == "102") {
        out = KeyboardType::Enhanced;
        return true;
    }
    return false;
}

const char* keyboard_type_name(KeyboardType type) {
    return type == KeyboardType::Terminal122 ? "122" : "enhanced";
}

uint8_t keyboard_id_first_byte(KeyboardType type) {
    return type == KeyboardType::Terminal122 ? 0xBF : 0x7F;
}

bool keyboard_layout_id(KeyboardType type, const std::string& layout, uint8_t& id) {
    const KeyboardLayoutInfo* l = find_keyboard_layout(layout);
    if (!l) return false;
    const uint8_t v = type == KeyboardType::Terminal122 ? l->id122 : l->id_enhanced;
    if (v == 0) return false;
    id = v;
    return true;
}

bool keyboard_id_bytes(KeyboardType type, const std::string& layout, uint8_t& id1, uint8_t& id2) {
    uint8_t id = 0;
    if (!keyboard_layout_id(type, layout, id)) return false;
    id1 = keyboard_id_first_byte(type);
    id2 = id;
    return true;
}

bool resolve_keyboard_id(const TerminalOptions& t, uint8_t& id1, uint8_t& id2, std::string& error) {
    KeyboardType type;
    if (!parse_keyboard_type(t.keyboard_type, type)) {
        error = "unknown keyboard type '" + t.keyboard_type + "' (use 122 or enhanced)";
        return false;
    }
    id1 = keyboard_id_first_byte(type);
    if (t.keyboard_id >= 0) {
        id2 = uint8_t(t.keyboard_id);
        return true;
    }
    if (!find_keyboard_layout(t.keyboard_layout)) {
        error = "unknown keyboard layout '" + t.keyboard_layout + "' (see --list-keyboards)";
        return false;
    }
    if (!keyboard_id_bytes(type, t.keyboard_layout, id1, id2)) {
        error = "no " + std::string(keyboard_type_name(type)) + " keyboard exists for layout '" +
                t.keyboard_layout + "'";
        return false;
    }
    return true;
}

std::string keyboard_layout_list() {
    std::string out = format("%-14s %-6s %-9s %s\n", "layout", "122", "enhanced", "description");
    for (const KeyboardLayoutInfo& l : keyboard_layouts()) {
        out += format("%-14s %-6s %-9s %s%s\n", l.name, l.id122 ? format("%02X", l.id122).c_str() : "-",
                      l.id_enhanced ? format("%02X", l.id_enhanced).c_str() : "-", l.description,
                      l.documented ? "" : " (inferred)");
    }
    return out;
}

} // namespace em34xx
