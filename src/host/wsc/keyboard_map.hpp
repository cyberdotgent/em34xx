// Scan codes from poll responses -> operator actions.
//
// Turning scan codes into characters is the controller's job; the terminal
// sends key positions [doc PAI 2.1]. The tables here are typed in from the
// 5250 Product Attachment Information (Oct 1989):
//   - IBM Enhanced Keyboard, fig. 147 (key numbers and scan codes), US
//     functions, with US characters in EBCDIC CCSID 37.
// The 122-key typewriter keyboard of the 3180-2/3197/3477 (fig. 105) and
// national layouts are extension points: their scan codes come back as
// OperatorAction::Kind::Unmapped with the raw scan code, so the engine can
// still act on them.
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
    void reset();

    // Feeds one poll byte 2 (non-zero, new response level). Returns false
    // for make/break of a shift-type key, which only changes state.
    bool translate(uint8_t scan, OperatorAction& out);

    bool shift() const { return lshift_ || rshift_; }
    bool alt() const { return alt_; }
    bool caps_lock() const { return caps_; }

private:
    bool enhanced() const { return (keyboard_id_ & 0x80) != 0; }

    uint8_t keyboard_id_;
    bool lshift_ = false;
    bool rshift_ = false;
    bool alt_ = false;
    bool caps_ = false;
};

} // namespace wsc
} // namespace tn34xx
