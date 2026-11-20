// Keyboard identification for IBM 34xx InfoWindow terminals.
//
// The terminal's language is selected by the keyboard it finds attached: the
// keyboard answers the Read ID command (0xF2) with ACK and two ID bytes. For
// IBM terminal keyboards the first byte is 0xBF (122/124-key) or 0x7F (IBM
// Enhanced 101/102-key terminal keyboard) and the second byte carries the
// country/layout. See docs/keyboard.md for sources and for which parts are
// documented and which inferred.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tn34xx {

enum class KeyboardType { Terminal122, Enhanced };

// One national keyboard. id122/id_enhanced are the layout ID bytes listed in
// the 3489 maintenance manual (SY18-2345); 0 means that no keyboard of that
// type exists for the layout. documented is false when a value was inferred
// rather than read from the manual.
struct KeyboardLayoutInfo {
    const char* name;          // short lowercase name, e.g. "us", "de"
    const char* description;
    uint8_t id122;
    uint8_t id_enhanced;
    bool documented;
};

const std::vector<KeyboardLayoutInfo>& keyboard_layouts();
const KeyboardLayoutInfo* find_keyboard_layout(const std::string& name);

// Accepts "122", "124", "terminal" and "enhanced", "101", "102" (any case).
bool parse_keyboard_type(const std::string& text, KeyboardType& out);
const char* keyboard_type_name(KeyboardType type);

// First ID byte for a keyboard type: 0xBF for 122-key, 0x7F for Enhanced
// (inferred; see docs/keyboard.md).
uint8_t keyboard_id_first_byte(KeyboardType type);

// Layout ID byte for type + layout; false if the layout is unknown or has no
// keyboard of that type.
bool keyboard_layout_id(KeyboardType type, const std::string& layout, uint8_t& id);

// Full two-byte reply to Read ID (0xF2), after the ACK; false if unknown.
bool keyboard_id_bytes(KeyboardType type, const std::string& layout, uint8_t& id1, uint8_t& id2);

struct TerminalOptions;

// Check the keyboard type/layout and work out the two Read ID bytes the
// emulated keyboard reports (an explicit keyboard_id replaces the second).
bool resolve_keyboard_id(const TerminalOptions& t, uint8_t& id1, uint8_t& id2, std::string& error);
std::string keyboard_layout_list();

} // namespace tn34xx
