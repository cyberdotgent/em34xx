// SPDX-License-Identifier: GPL-3.0-or-later
// The 3270 presentation space of one TN3270 session, and the 3270 data
// stream that updates it.
//
// Pure state machine: host records go in, the screen and the records to
// send back come out. No sockets, no coax: the CUT controller
// (src/host/coax/) paints a terminal from this and feeds operator actions
// back in, as a 3174 does for its terminals.
//
// Implemented from the 3270 Data Stream Programmer's Reference (GA23-0059)
// [documented]:
//   commands   Write, Erase/Write, Erase/Write Alternate, Erase All
//              Unprotected, Read Buffer, Read Modified (All), Write
//              Structured Field (Read Partition Query/Query List, Outbound
//              3270DS for the implicit partition)
//   orders     SF, SFE, SBA, SA, MF, IC, PT, RA, EUA, GE
//   attributes field attribute (protected, numeric, display, MDT);
//              extended: highlighting, foreground colour, character set
//   inbound    AID records (short read for PA/Clear, modified fields
//              otherwise), Read Buffer, Query Reply
// The screen is 24x80 (model 2) by default; the alternate size is the
// model's (Erase/Write Alternate).
#pragma once

#include <cstdint>
#include <vector>

namespace em34xx {
namespace tn3270 {

// Attention identifiers [documented].
enum class Aid : uint8_t {
    None = 0x60,
    Enter = 0x7D,
    Clear = 0x6D,
    PA1 = 0x6C,
    PA2 = 0x6E,
    PA3 = 0x6B,
    SysReq = 0xF0,
    // PF1-PF24 are pf_aid(n).
};
uint8_t pf_aid(int n);   // 1..24

// Operator functions that do not send anything by themselves.
enum class Key {
    Tab, BackTab, Home, NewLine, Left, Right, Up, Down, Backspace, Delete, Insert,
    EraseEOF, EraseInput, Dup, FieldMark, Reset,
};

struct Cell {
    uint8_t ch = 0x00;          // EBCDIC character, or the field attribute byte when fa
    bool fa = false;            // this position holds a field attribute
    uint8_t highlight = 0;      // 00 default, F1 blink, F2 reverse, F4 underscore
    uint8_t colour = 0;         // 00 default, F1 blue .. F7 white
    uint8_t charset = 0;        // 00 default, F1 APL/GE
};

// Field attribute bits [documented].
inline bool fa_protected(uint8_t a) { return (a & 0x20) != 0; }
inline bool fa_numeric(uint8_t a) { return (a & 0x10) != 0; }
inline bool fa_intensified(uint8_t a) { return (a & 0x0C) == 0x08; }
inline bool fa_nondisplay(uint8_t a) { return (a & 0x0C) == 0x0C; }
inline bool fa_modified(uint8_t a) { return (a & 0x01) != 0; }

// 3270 buffer address <-> the two-byte 12/14-bit encodings.
void encode_address(int addr, uint8_t& b1, uint8_t& b2);
int decode_address(uint8_t b1, uint8_t b2);

class Engine {
public:
    // Alternate (Erase/Write Alternate) size; default model 2 has 24x80 only.
    explicit Engine(int alt_rows = 24, int alt_cols = 80);

    // A host record (one 3270 data stream message, telnet framing removed).
    // Records to send back (Read Buffer, Read Modified, Query Reply) are
    // appended to `out`.
    void process(const std::vector<uint8_t>& record, std::vector<std::vector<uint8_t>>& out);

    int rows() const { return rows_; }
    int cols() const { return cols_; }
    int size() const { return rows_ * cols_; }
    const Cell& cell(int addr) const { return buf_[size_t(addr)]; }
    int cursor() const { return cursor_; }
    // The field attribute in effect at addr (the nearest one at or before
    // it, wrapping); 0 when the screen is unformatted.
    uint8_t field_attr(int addr) const;
    bool formatted() const;

    // Operator side. type() enters an EBCDIC character at the cursor;
    // returns false (and sets the keyboard to "operator error") when the
    // position does not take it.
    bool type(uint8_t ebcdic);
    void key(Key k);
    // An attention key: the inbound record goes to `out`, and the keyboard
    // locks until the host restores it.
    void aid(uint8_t aid, std::vector<std::vector<uint8_t>>& out);

    // Status for the operator information area.
    bool keyboard_locked() const { return locked_; }
    bool insert_mode() const { return insert_; }
    bool operator_error() const { return opererr_; }
    unsigned take_alarms() { unsigned n = alarms_; alarms_ = 0; return n; }
    uint64_t generation() const { return gen_; }

    // The Query Reply this terminal answers with (public for tests).
    std::vector<uint8_t> query_reply() const;

private:
    void erase(int rows, int cols);
    void write(const uint8_t* p, size_t n, bool erase_first, int rows, int cols);
    void apply_wcc(uint8_t wcc);
    void read_buffer(std::vector<std::vector<uint8_t>>& out) const;
    void read_modified(uint8_t aid, bool all, std::vector<std::vector<uint8_t>>& out) const;
    void structured_fields(const uint8_t* p, size_t n, std::vector<std::vector<uint8_t>>& out);
    int field_start(int addr) const;    // address of the FA governing addr, or -1
    int next_unprotected(int from) const;
    void set_mdt(int addr);
    void touch() { ++gen_; }

    int alt_rows_, alt_cols_;
    int rows_ = 24, cols_ = 80;
    std::vector<Cell> buf_;
    int cursor_ = 0;
    bool locked_ = false;
    bool insert_ = false;
    bool opererr_ = false;
    unsigned alarms_ = 0;
    uint64_t gen_ = 0;
    uint8_t last_aid_ = uint8_t(Aid::None);
};

} // namespace tn3270
} // namespace em34xx
