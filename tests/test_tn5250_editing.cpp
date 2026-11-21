// SPDX-License-Identifier: GPL-3.0-or-later
// Operator field editing, keyboard states and error codes (FR chapter 16,
// FFW semantics FR 15.6.12.1).
#include "tn5250_test_util.hpp"

#include <doctest/doctest.h>

using namespace tn5250_test;

namespace {
// Field data as ASCII (nulls as '_').
std::string field_text(const Host& h, int fi = 0) {
    const Field& f = h.e.fields()[size_t(fi)];
    std::string s;
    for (int a = f.start; a < f.end(); ++a) {
        const uint8_t b = h.e.cell(a / h.e.cols(), a % h.e.cols()).ch;
        if (b == 0x00) s += '_';
        else if (b == 0x1C) s += '*';
        else if (b == 0x60) s += '-';
        else if (b >= 0xF0 && b <= 0xF9) s += char('0' + (b - 0xF0));
        else if (b >= 0xC1 && b <= 0xC9) s += char('A' + (b - 0xC1));
        else if (b >= 0xD1 && b <= 0xD9) s += char('J' + (b - 0xD1));
        else if (b >= 0xE2 && b <= 0xE9) s += char('S' + (b - 0xE2));
        else if (b == 0x40) s += ' ';
        else s += '?';
    }
    return s;
}
} // namespace

TEST_CASE("tn5250 editing: keys while locked are ignored; clicker follows the lock") {
    Host h;
    h.write({0x04, 0x40, 0x04, 0x11, 0x00, 0x00, 0x11, 0x01, 0x01, 0x1D, 0x40, 0x00, 0x24, 0x00, 0x05});
    CHECK(h.e.indicators().keyboard_locked);
    CHECK_FALSE(h.e.indicators().clicker);
    CHECK(h.e.key_data(0xC1) == KeyResult::Ignored);
    CHECK(h.e.key_aid(Aid::Enter) == KeyResult::Ignored);
    CHECK(h.e.key_function(Key::Attention) == KeyResult::Accepted);   // Attn works locked
    const Record atn = h.take();
    CHECK(atn.flags == kFlagAtn);
    CHECK(atn.data.empty());
    h.write({0x04, 0x11, 0x00, 0x08});
    CHECK(h.e.indicators().clicker);
    CHECK(h.e.cursor_col() == 1);  // first non-bypass field without an IC
}

TEST_CASE("tn5250 editing: protected area posts 0005 on the error line; Reset restores") {
    Host h;
    h.write(signon_stream());
    h.e.key_function(Key::CursorUp);
    CHECK(h.e.key_data(0xC1) == KeyResult::Error);
    const Indicators ind = h.e.indicators();
    CHECK(ind.error == ErrorState::PreHelp);
    CHECK(ind.error_code == 0x0005);
    CHECK(ind.input_inhibited);
    CHECK_FALSE(ind.clicker);
    CHECK(h.row(23) == " 0005");
    CHECK(h.e.take_alarms() == 1);
    CHECK(h.e.key_function(Key::CursorDown) == KeyResult::Ignored);
    CHECK(h.e.key_function(Key::Help) == KeyResult::Accepted);
    CHECK(h.take().data == Bytes{0x00, 0x05});
    CHECK(h.e.key_function(Key::Reset) == KeyResult::Accepted);
    CHECK(h.row(23).empty());
    CHECK_FALSE(h.e.indicators().input_inhibited);
}

TEST_CASE("tn5250 editing: field shift/edit checks") {
    SUBCASE("alpha only") {
        Host h;
        h.write(one_field(1, 1, 0x4100, 5));
        CHECK(h.e.key_data(0xC1) == KeyResult::Accepted);
        CHECK(h.e.key_data(0x6B) == KeyResult::Accepted);   // comma
        CHECK(h.e.key_data(0xF1) == KeyResult::Error);
        CHECK(h.e.indicators().error_code == kErrAlphaOnly);
    }
    SUBCASE("numeric only") {
        Host h;
        h.write(one_field(1, 1, 0x4300, 5));
        CHECK(h.e.key_data(0xF1) == KeyResult::Accepted);
        CHECK(h.e.key_data(0x4E) == KeyResult::Accepted);   // plus
        CHECK(h.e.key_data(0xC1) == KeyResult::Error);
        CHECK(h.e.indicators().error_code == kErrNumericOnly);
    }
    SUBCASE("digits only") {
        Host h;
        h.write(one_field(1, 1, 0x4500, 5));
        CHECK(h.e.key_data(0x4E) == KeyResult::Error);
        CHECK(h.e.indicators().error_code == kErrDigitsOnly);
    }
    SUBCASE("I/O field accepts no keyboard data") {
        Host h;
        h.write(one_field(1, 1, 0x4600, 5));
        CHECK(h.e.key_data(0xF1) == KeyResult::Error);
    }
    SUBCASE("bypass field") {
        Host h;
        h.write(one_field(1, 1, 0x6000, 5));
        h.e.key_function(Key::CursorRight);
        CHECK(h.e.key_data(0xF1) == KeyResult::Error);
        CHECK(h.e.indicators().error_code == kErrProtected);
    }
}

TEST_CASE("tn5250 editing: signed numeric, Field- and Field+") {
    Host h;
    h.write(one_field(1, 1, 0x4700, 5));      // 4 digits + sign
    h.type("12");
    CHECK(h.e.key_function(Key::FieldMinus) == KeyResult::Accepted);
    CHECK(field_text(h) == "__12-");
    h.write({0x04, 0x52, 0x00, 0x00});
    h.e.key_aid(Aid::Enter);
    const Bytes d = h.take().data;
    CHECK(Bytes(d.begin() + 3, d.end()) == Bytes{0x11, 0x01, 0x02, 0x40, 0x40, 0xF1, 0xD2});

    Host p;
    p.write(one_field(1, 1, 0x4700, 5));
    p.type("1234");                            // fills the digits and wraps to the field start
    CHECK(field_text(p) == "1234_");
    for (int k = 0; k < 4; ++k) p.e.key_function(Key::CursorRight);
    CHECK(p.e.key_data(0xF5) == KeyResult::Error);
    CHECK(p.e.indicators().error_code == kErrSignPosition);
    p.e.key_function(Key::Reset);
    p.e.key_function(Key::CursorLeft);
    CHECK(p.e.key_function(Key::FieldPlus) == KeyResult::Accepted);
    CHECK(field_text(p) == "_123 ");
}

TEST_CASE("tn5250 editing: Field- on numeric-only sets the D zone; on alpha it is an error") {
    Host h;
    h.write(one_field(1, 1, 0x4300, 4));
    h.type("42");
    CHECK(h.e.key_function(Key::FieldMinus) == KeyResult::Accepted);
    CHECK(h.e.cell(0, 2).ch == 0xD2);
    Host a;
    a.write(one_field(1, 1, 0x4000, 4));
    a.type("A");
    CHECK(a.e.key_function(Key::FieldMinus) == KeyResult::Error);
    CHECK(a.e.indicators().error_code == kErrFieldMinus);
}

TEST_CASE("tn5250 editing: right adjust with zero and blank fill") {
    Host z;
    z.write(one_field(1, 1, 0x4005, 5));
    z.type("12");
    z.e.key_function(Key::FieldExit);
    CHECK(field_text(z) == "00012");
    CHECK(z.e.fields()[0].mdt());
    Host b;
    b.write(one_field(1, 1, 0x4006, 5));
    b.type("12");
    b.e.key_function(Key::FieldExit);
    CHECK(field_text(b) == "   12");
}

TEST_CASE("tn5250 editing: insert, delete, Erase EOF, Erase Input") {
    Host h;
    h.write(one_field(1, 1, 0x4000, 5));
    h.type("ABC");
    h.e.key_function(Key::FieldBackspace);
    CHECK(h.e.cursor_col() == 1);
    h.e.key_function(Key::Insert);
    CHECK(h.e.indicators().insert_mode);
    h.type("X");
    CHECK(field_text(h) == "XABC_");
    h.type("Y");
    CHECK(field_text(h) == "XYABC");
    CHECK(h.e.key_data(0xE9) == KeyResult::Error);
    CHECK(h.e.indicators().error_code == kErrInsertFull);
    h.e.key_function(Key::Reset);
    CHECK_FALSE(h.e.indicators().insert_mode);   // errors leave insert mode
    h.e.key_function(Key::Delete);
    CHECK(field_text(h) == "XYBC_");
    h.e.key_function(Key::EraseEof);
    CHECK(field_text(h) == "XY___");
    h.e.key_function(Key::EraseInput);
    CHECK(field_text(h) == "_____");
    CHECK(h.e.cursor_col() == 1);
}

TEST_CASE("tn5250 editing: mandatory enter and mandatory fill") {
    Host h;
    h.write({0x04, 0x40, 0x04, 0x11, 0x00, 0x08,
             0x11, 0x01, 0x01, 0x1D, 0x40, 0x08, 0x24, 0x00, 0x03,    // mandatory enter
             0x11, 0x02, 0x01, 0x1D, 0x40, 0x00, 0x24, 0x00, 0x03,
             0x04, 0x52, 0x00, 0x00});
    CHECK(h.e.key_function(Key::FieldExit) == KeyResult::Error);
    CHECK(h.e.indicators().error_code == kErrMandatoryEnter);
    h.e.key_function(Key::Reset);
    h.e.key_function(Key::FieldAdvance);
    h.type("A");
    CHECK(h.e.key_aid(Aid::Enter) == KeyResult::Error);
    CHECK(h.e.indicators().error_code == kErrMandatoryEnter);
    CHECK(h.e.cursor_row() == 0);              // moved to the field that needs data
    CHECK(h.out.empty());

    Host f;
    f.write(one_field(1, 1, 0x4007, 4));
    f.type("AB");
    CHECK(f.e.key_function(Key::FieldExit) == KeyResult::Error);
    CHECK(f.e.indicators().error_code == kErrMandatoryFill);
    f.e.key_function(Key::Reset);
    f.type("CD");
    CHECK(field_text(f) == "ABCD");
}

TEST_CASE("tn5250 editing: Dup and Field Mark need the FFW enable bit") {
    Host n;
    n.write(one_field(1, 1, 0x4000, 4));
    CHECK(n.e.key_function(Key::Dup) == KeyResult::Error);
    CHECK(n.e.indicators().error_code == kErrDupNotAllowed);
    n.e.key_function(Key::Reset);
    CHECK(n.e.key_function(Key::FieldMark) == KeyResult::Error);
    Host d;
    d.write(one_field(1, 1, 0x5000, 4));
    d.type("A");
    CHECK(d.e.key_function(Key::Dup) == KeyResult::Accepted);
    CHECK(field_text(d) == "A***");
    CHECK(d.e.fields()[0].mdt());
}

TEST_CASE("tn5250 editing: Field Exit required and auto enter") {
    Host h;
    h.write(one_field(1, 1, 0x4040, 2));
    h.type("AB");
    CHECK(h.e.cursor_col() == 2);              // stays under the last character
    CHECK(h.e.key_data(0xC3) == KeyResult::Error);
    h.e.key_function(Key::Reset);
    CHECK(h.e.key_function(Key::FieldExit) == KeyResult::Accepted);
    CHECK(field_text(h) == "AB");

    Host a;
    a.write(one_field(1, 1, 0x4080, 2));
    a.type("AB");
    REQUIRE(a.out.size() == 1);
    const Record r = a.take();
    CHECK(r.opcode == kOpPutGet);
    CHECK(r.data[2] == 0xF1);
    CHECK(Bytes(r.data.begin() + 3, r.data.end()) == cat({{0x11, 0x01, 0x02}, eb("AB")}));
}

TEST_CASE("tn5250 editing: cursor movement, Home and Record Backspace") {
    Host h;
    h.write(signon_stream());
    h.e.key_function(Key::FieldAdvance);
    CHECK(h.e.cursor_row() == 6);
    h.e.key_function(Key::FieldAdvance);       // wraps
    CHECK(h.e.cursor_row() == 5);
    h.e.key_function(Key::FieldBackspace);     // previous field (wraps backwards)
    CHECK(h.e.cursor_row() == 6);
    h.e.key_function(Key::CursorLeft);
    h.e.key_function(Key::NewLine);            // no field below row 7: first field
    CHECK(h.e.cursor_row() == 5);
    CHECK(h.e.cursor_col() == 52);
    h.e.key_function(Key::CursorDown);
    h.e.key_function(Key::Home);
    CHECK(h.e.cursor_row() == 5);              // the IC
    CHECK(h.out.empty());
    h.e.key_function(Key::Home);               // already home: Record Backspace
    REQUIRE(h.out.size() == 1);
    CHECK(h.take().data == Bytes{0x06, 0x35, 0xF8});
    h.write({0x04, 0x11, 0x00, 0x08});
    for (int k = 0; k < 80; ++k) h.e.key_function(Key::CursorRight);
    CHECK(h.e.cursor_row() == 6);
    h.e.key_function(Key::CursorUp);
    h.e.key_function(Key::CursorUp);
    h.e.key_function(Key::CursorUp);
    h.e.key_function(Key::CursorUp);
    h.e.key_function(Key::CursorUp);
    h.e.key_function(Key::CursorUp);
    h.e.key_function(Key::CursorUp);
    CHECK(h.e.cursor_row() == 23);             // wraps top to bottom
}

TEST_CASE("tn5250 editing: an AID without a pending read is held until the next read") {
    Host h;
    h.write(one_field(1, 1, 0x4000, 4));
    h.write({0x04, 0x11, 0x00, 0x00});         // replaces nothing; read still pending
    h.e.key_aid(Aid::Enter);
    h.take();                                  // consumed the read
    h.write({0x04, 0x11, 0x00, 0x08});         // unlock, no read
    h.type("Q");
    CHECK(h.e.key_aid(Aid::Enter) == KeyResult::Accepted);
    CHECK(h.out.empty());
    CHECK(h.e.pending_aid() == 0xF1);
    h.write({0x04, 0x52, 0x00, 0x00});
    REQUIRE(h.out.size() == 1);
    CHECK(h.take().data == cat({{0x01, 0x03, 0xF1, 0x11, 0x01, 0x02}, eb("Q")}));
}

TEST_CASE("tn5250 editing: Sys Req and Help outside an error state") {
    Host h;
    h.write(signon_stream());
    CHECK(h.e.key_function(Key::SysReq) == KeyResult::Accepted);
    const Record s = h.take();
    CHECK(s.flags == kFlagSrq);
    CHECK(s.data.empty());
    CHECK(h.e.key_function(Key::Help) == KeyResult::Accepted);
    CHECK(h.take().data == Bytes{0x06, 0x35, 0xF3});
}

TEST_CASE("tn5250 editing: device return marks the whole screen dirty") {
    Host h;
    h.write(signon_stream());
    h.e.take_dirty();
    h.e.set_device_ready(false);
    CHECK(h.e.take_dirty().empty());
    const uint64_t g = h.e.generation();
    h.e.set_device_ready(true);
    const Rect r = h.e.take_dirty();
    CHECK(r.top == 0);
    CHECK(r.bottom == 23);
    CHECK(r.right == 79);
    CHECK(h.e.generation() > g);
}
