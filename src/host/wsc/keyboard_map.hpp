// Scan codes from poll responses -> operator actions.
//
// Turning scan codes into characters is the controller's job; the terminal
// sends key positions [doc PAI 2.1]. What the cable carries depends on the
// keyboard (docs/keyboard.md, "5250 scan codes on the cable"):
//   - IBM Enhanced Keyboard (keyboard ID bit 80h set): the keyboard's set-3
//     make code, bit 7 = break; table typed in from the 5250 Product
//     Attachment Information (Oct 1989), fig. 147;
//   - 122-key typewriter keyboard (keyboard ID 02h, 0Ah world trade): the
//     3180-2/3196 codes of PAI fig. 105 (5251-compatible: Shift as make/break
//     codes 57h/56h, Shift Lock 54h, a CMD prefix 6Fh for Alt and F1-F24).
//     They are decoded back to key positions (set-3 codes), and the legends
//     come from the 3489 User's Guide.
// Both use US functions and US characters in EBCDIC CCSID 37; national
// graphics are an extension point. Keys without a known legend come back as
// OperatorAction::Kind::Unmapped with the raw scan code.
#pragma once

#include "host/wsc/session_port.hpp"

#include <cstdint>

namespace tn34xx {
namespace wsc {

class KeyboardMap {
public:
    // keyboard_id: the Read Device ID (keyboard) answer; bit 80h = Enhanced.
    explicit KeyboardMap(uint8_t keyboard_id = 0x82) : keyboard_id_(keyboard_id) {}
    void set_keyboard_id(uint8_t id) { keyboard_id_ = id; reset(); }
    uint8_t keyboard_id() const { return keyboard_id_; }
    bool enhanced_layout() const { return enhanced(); }
    void reset();

    // Feeds one poll byte 2 (non-zero, new response level). Returns false
    // for make/break of a shift-type key, which only changes state.
    bool translate(uint8_t scan, OperatorAction& out);

    bool shift() const { return lshift_ || rshift_; }
    bool alt() const { return alt_; }
    bool caps_lock() const { return caps_; }   // Shift Lock on the typewriter keyboard
    bool cmd_pending() const { return cmd_; }   // typewriter: CMD prefix awaiting its key

private:
    bool translate_typewriter(uint8_t key, bool brk, OperatorAction& out);
    bool enhanced() const { return (keyboard_id_ & 0x80) != 0; }

    uint8_t keyboard_id_;
    bool lshift_ = false;
    bool rshift_ = false;
    bool alt_ = false;
    bool caps_ = false;
    bool cmd_ = false;   // typewriter: CMD prefix (6Fh) seen
};

} // namespace wsc
} // namespace tn34xx
