// Interface between the TN5250 side of the bridge and its consumer.
//
// The consumer is the virtual workstation controller (src/host/wsc/), which
// paints the emulated terminal from the shadow screen and feeds operator
// keystrokes back in. Nothing here knows about Twinax, device commands or
// scan codes: the shadow is the 5250 presentation space as the IBM 5494
// Functions Reference (SC30-3533-04, ch. 15-16) describes it, and the input
// entry points are the 5250 operator functions, not keys.
//
// Threading: a DisplaySession is single-threaded. The networked
// implementation (tn5250/client.hpp) receives records on its own I/O thread
// and only applies them inside pump(), which the owner calls from the thread
// that also reads the shadow and delivers keys (the emulation thread).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tn34xx {
namespace tn5250 {

// ---------------------------------------------------------------------------
// Shadow screen

// One presentation-space position.
//
// `ch` is the byte held in the regeneration buffer: 0x00 null, 0x20-0x3F a
// field attribute (it occupies the position and shows as a blank), anything
// else an EBCDIC character (0x1C Dup and 0x1E Field Mark show as overstruck
// '*' and ';' on a real station). `attr` is the field attribute in effect at
// this position, i.e. the nearest attribute byte at or before it in reading
// order (0x20 "normal" when there is none). `ext_primary` and `ext_color`
// are the Write Extended Attribute values (FR 15.6.11.1/2; 0 = none).
struct Cell {
    uint8_t ch = 0x00;
    uint8_t attr = 0x20;
    uint8_t ext_primary = 0;
    uint8_t ext_color = 0;

    bool is_attribute() const { return ch >= 0x20 && ch <= 0x3F; }
    bool operator==(const Cell& o) const {
        return ch == o.ch && attr == o.attr && ext_primary == o.ext_primary && ext_color == o.ext_color;
    }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

// Field-attribute helpers (FR Table 51; bits counted with 0x80 = bit 0).
inline bool attr_column_separator(uint8_t a) { return (a & 0x10) != 0; }
inline bool attr_blink(uint8_t a) { return (a & 0x08) != 0 && (a & 0x07) != 0x07; }
inline bool attr_underscore(uint8_t a) { return (a & 0x04) != 0 && (a & 0x07) != 0x07; }
inline bool attr_high_intensity(uint8_t a) { return (a & 0x02) != 0 && (a & 0x07) != 0x07; }
inline bool attr_reverse(uint8_t a) { return (a & 0x01) != 0 && (a & 0x07) != 0x07; }
inline bool attr_nondisplay(uint8_t a) { return (a & 0x07) == 0x07; }

// Rows/columns are 0-based throughout this interface; the data stream's own
// 1-based addresses never leak out of the engine.
struct Rect {
    int top = 0, left = 0, bottom = -1, right = -1;  // inclusive; empty if bottom < top
    bool empty() const { return bottom < top || right < left; }
    void add(int row, int col) {
        if (empty()) { top = bottom = row; left = right = col; return; }
        if (row < top) top = row;
        if (row > bottom) bottom = row;
        if (col < left) left = col;
        if (col > right) right = col;
    }
};

// Operator error states (FR 16.1). PreHelp is entered by a keying error or
// a host Write Error Code; Help moves it to PostHelp; Reset leaves both.
enum class ErrorState : uint8_t { None, PreHelp, PostHelp };

struct Indicators {
    bool system_available = false;   // session up (5250 "System Available")
    bool message_waiting = false;
    bool input_inhibited = true;     // keyboard locked or in error state
    bool keyboard_locked = true;     // "normal locked" (waiting for the host)
    bool insert_mode = false;
    bool cursor_blink = false;       // WTD CC1 / error state cursor blink
    ErrorState error = ErrorState::None;
    uint16_t error_code = 0;         // operator error code (BCD-style, e.g. 0x0005)
    bool system_request = false;     // Sys Req line active
};

struct ScreenSnapshot {
    int rows = 24, cols = 80;
    std::vector<Cell> cells;         // rows*cols, row-major
    int cursor_row = 0, cursor_col = 0;
    Indicators indicators;
    uint64_t generation = 0;
    const Cell& at(int r, int c) const { return cells[size_t(r) * size_t(cols) + size_t(c)]; }
};

// ---------------------------------------------------------------------------
// Operator input

// Attention identifier codes (FR Table 109).
enum class Aid : uint8_t {
    None = 0x00,
    Clear = 0xBD,
    Enter = 0xF1,
    Help = 0xF3,
    RollDown = 0xF4,          // Page Up
    RollUp = 0xF5,            // Page Down
    RollLeft = 0xD9,
    RollRight = 0xDA,
    Print = 0xF6,
    RecordBackspace = 0xF8,
    AutoEnterSlp = 0x3F,
    ForwardEdgeTrigger = 0x50,
    PA1 = 0x6C,
    PA2 = 0x6E,
    PA3 = 0x6B,
    F1 = 0x31, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    F13 = 0xB1, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24,
    QueryReply = 0x88,        // inbound only
};
// F1..F24 as an Aid; n outside 1..24 gives Aid::None.
Aid function_key_aid(int n);

// Non-AID operator functions (FR 16.2.2-16.3).
enum class Key : uint8_t {
    FieldExit,
    FieldPlus,
    FieldMinus,
    FieldAdvance,      // Tab
    FieldBackspace,    // Backtab
    CursorUp,
    CursorDown,
    CursorLeft,
    CursorRight,
    Backspace,         // destructive-free cursor left within input
    Home,
    NewLine,
    Insert,            // toggles insert mode
    Delete,
    EraseEof,
    EraseInput,
    Dup,
    FieldMark,
    Reset,             // Error Reset
    Help,              // Help: AID F3 normally, error help in error state
    Attention,
    SysReq,
};

const char* key_name(Key k);

// What the engine did with a key, so the controller can sound the alarm or
// repaint selectively.
enum class KeyResult : uint8_t {
    Accepted,          // state changed or an AID/signal was sent
    Ignored,           // e.g. data key while the keyboard is locked
    Error,             // operator error posted (see Indicators::error_code)
};

// ---------------------------------------------------------------------------
// Records exchanged with the host (RFC 1205 section 3 header, minus framing)

enum RecordFlags : uint8_t {
    kFlagErr = 0x80,   // data stream output error / negative response
    kFlagAtn = 0x40,   // Attention key
    kFlagSrq = 0x04,   // System Request key
    kFlagTrq = 0x02,   // Test Request key
    kFlagHlp = 0x01,   // Help in error state
};

enum Opcode : uint8_t {
    kOpNoOp = 0x00,
    kOpInvite = 0x01,
    kOpOutputOnly = 0x02,
    kOpPutGet = 0x03,
    kOpSaveScreen = 0x04,
    kOpRestoreScreen = 0x05,
    kOpReadImmediate = 0x06,
    kOpReadScreen = 0x08,
    kOpCancelInvite = 0x0A,
    kOpMessageOn = 0x0B,
    kOpMessageOff = 0x0C,
};

struct Record {
    uint8_t opcode = kOpNoOp;
    uint8_t flags = 0;
    std::vector<uint8_t> data;       // 5250 data stream after the header
};

// ---------------------------------------------------------------------------
// The session as the controller sees it

class DisplaySession {
public:
    virtual ~DisplaySession() = default;

    // Apply anything that arrived from the host. Must be called regularly
    // from the consumer thread; cheap when there is nothing to do.
    virtual void pump() = 0;

    // --- shadow screen ---
    virtual int rows() const = 0;
    virtual int cols() const = 0;
    virtual const Cell& cell(int row, int col) const = 0;
    virtual int cursor_row() const = 0;
    virtual int cursor_col() const = 0;
    virtual Indicators indicators() const = 0;
    // Bumped on every change to cells, cursor, size or indicators.
    virtual uint64_t generation() const = 0;
    // Region changed since the last call (cells only; a size change marks
    // the whole screen). Clears the accumulated region.
    virtual Rect take_dirty() = 0;
    // Number of alarms requested by the host (WTD CC1) or by operator
    // errors since the last call.
    virtual unsigned take_alarms() = 0;
    virtual void snapshot(ScreenSnapshot& out) const = 0;

    // --- operator input ---
    virtual KeyResult key_data(uint8_t ebcdic) = 0;
    virtual KeyResult key_function(Key key) = 0;
    virtual KeyResult key_aid(Aid aid) = 0;

    // --- host replies ---
    // The engine produces inbound records (AID replies, query replies,
    // Attention / Sys Req signals, negative responses). A networked session
    // sends them itself; a bare engine hands them to this sink, which may be
    // replaced (for example by a test harness).
    using RecordSink = std::function<void(const Record&)>;
    virtual void set_record_sink(RecordSink sink) = 0;
};

} // namespace tn5250
} // namespace tn34xx
