// SPDX-License-Identifier: GPL-3.0-or-later
// 5250 data stream parser and writer, from hand-written streams built from
// the 5494 Functions Reference (SC30-3533-04) chapter 15 and RFC 1205.
#include "tn5250_test_util.hpp"

#include <doctest/doctest.h>

using namespace tn5250_test;

namespace {
uint32_t sense_of(const Record& r) {
    REQUIRE(r.data.size() == 4);
    return (uint32_t(r.data[0]) << 24) | (uint32_t(r.data[1]) << 16) | (uint32_t(r.data[2]) << 8) | r.data[3];
}
} // namespace

TEST_CASE("tn5250 datastream: sign-on panel, typing and the Read MDT reply") {
    Host h;
    h.write(signon_stream());
    CHECK(h.out.empty());
    CHECK(h.e.rows() == 24);
    CHECK(h.row(0) == "                                   Sign On");
    REQUIRE(h.e.fields().size() == 2);
    const Field& user = h.e.fields()[0];
    CHECK(user.start == 5 * 80 + 52);
    CHECK(user.length == 10);
    CHECK(user.monocase());
    CHECK(h.e.cell(5, 51).ch == 0x24);   // leading attribute
    CHECK(h.e.cell(5, 62).ch == 0x20);   // ending attribute
    CHECK(h.e.cell(5, 55).attr == 0x24); // attribute in effect
    CHECK(h.e.cell(6, 51).ch == 0x27);
    CHECK(h.e.cursor_row() == 5);
    CHECK(h.e.cursor_col() == 52);
    CHECK_FALSE(h.e.indicators().keyboard_locked);
    CHECK(h.e.read_pending());

    h.type("qsecofr");                   // monocase: stored upper case
    CHECK(h.row(5).substr(52) == "QSECOFR");
    CHECK(h.e.key_function(Key::FieldExit) == KeyResult::Accepted);
    CHECK(h.e.cursor_row() == 6);
    CHECK(h.e.cursor_col() == 52);
    h.type("secret");
    CHECK(h.e.key_aid(Aid::Enter) == KeyResult::Accepted);
    REQUIRE(h.out.size() == 1);
    const Record r = h.take();
    CHECK(r.opcode == kOpPutGet);
    CHECK(r.data == cat({{0x07, 0x3B, 0xF1},
                         {0x11, 0x06, 0x35}, eb("QSECOFR"),
                         {0x11, 0x07, 0x35}, eb("secret")}));
    CHECK(h.e.indicators().keyboard_locked);
    CHECK_FALSE(h.e.read_pending());
}

TEST_CASE("tn5250 datastream: 0x04 inside order operands does not end the command") {
    Host h;
    h.write({0x04, 0x40, 0x04, 0x11, 0x00, 0x00,
             0x11, 0x04, 0x04, 0xC1,          // SBA row 4 col 4, 'A'
             0x02, 0x04, 0x0A, 0x5C,          // RA to row 4 col 10 with '*'
             0x11, 0x05, 0x01, 0xC2});        // SBA row 5 col 1, 'B'
    CHECK(h.out.empty());
    CHECK(h.row(3) == "   A******");
    CHECK(h.row(4) == "B");
}

TEST_CASE("tn5250 datastream: SOH is skipped by its length (error row 0x10 is not TD)") {
    Host h;
    h.write({0x04, 0x40, 0x04, 0x11, 0x00, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x10, 0xC1});
    CHECK(h.out.empty());
    CHECK(h.e.cell(0, 0).ch == 0xC1);
    CHECK(h.e.error_row() == 15);
    h.write({0x04, 0x11, 0x00, 0x00, 0x01, 0x00});
    REQUIRE(h.out.size() == 1);
    CHECK(sense_of(h.take()) == kSenseSohLength);
}

TEST_CASE("tn5250 datastream: TD, low controls, EA, WEA") {
    Host h;
    h.write({0x04, 0x40, 0x04, 0x11, 0x00, 0x00,
             0x11, 0x01, 0x01, 0x10, 0x00, 0x03, 0x04, 0x11, 0x1D, 0xC1,   // TD of 3 raw bytes, then 'A'
             0x11, 0x02, 0x01, 0x05, 0xC2});                                // 0x05 takes one position
    CHECK(h.out.empty());
    CHECK(h.e.cell(0, 0).ch == 0x04);
    CHECK(h.e.cell(0, 1).ch == 0x11);
    CHECK(h.e.cell(0, 2).ch == 0x1D);
    CHECK(h.e.cell(0, 3).ch == 0xC1);
    CHECK(h.e.cell(1, 0).ch == 0x05);
    CHECK(h.e.cell(1, 1).ch == 0xC2);
    h.write(cat({{0x04, 0x11, 0x00, 0x00, 0x11, 0x03, 0x01}, eb("ABCDEF"),
                 {0x11, 0x03, 0x02, 0x03, 0x03, 0x04, 0x02, 0x00},       // EA to 3,4: screen
                 {0x11, 0x03, 0x06, 0x12, 0x01, 0x84, 0x12, 0x03, 0x88}}));  // WEA underscore, red at 3,6
    CHECK(h.out.empty());
    CHECK(h.row(2) == "A   EF");
    CHECK(h.e.cell(2, 1).ch == 0x00);
    CHECK(h.e.cell(2, 5).ext_primary == 0x84);
    CHECK(h.e.cell(2, 5).ext_color == 0x88);
    h.write({0x04, 0x11, 0x00, 0x00, 0x12, 0x05, 0x80});
    CHECK(sense_of(h.take()) == kSenseExtAttrType);
}

TEST_CASE("tn5250 datastream: Write Error Code has no control bytes; Help and Reset") {
    Host h;
    h.write(signon_stream());
    h.write({0x04, 0x21, 0x13, 0x07, 0x35, 0x22, 0xC5, 0xD9, 0xD9, 0x27});
    CHECK(h.out.empty());
    CHECK(h.e.cell(23, 0).ch == 0x22);
    CHECK(h.row(23) == " ERR");
    CHECK(h.e.cursor_row() == 6);        // IC moved the cursor only
    Indicators ind = h.e.indicators();
    CHECK(ind.error == ErrorState::PreHelp);
    CHECK(ind.input_inhibited);
    CHECK(h.e.key_data(0xC1) == KeyResult::Ignored);
    CHECK(h.e.key_function(Key::Help) == KeyResult::Accepted);
    const Record help = h.take();
    CHECK(help.flags == kFlagHlp);
    CHECK(help.data == Bytes{0x59, 0x97}); // columns 2-5 dezoned: C5 D9 D9 27 -> 5 9 9 7
    CHECK(h.e.indicators().error == ErrorState::PostHelp);
    CHECK(h.e.key_function(Key::Reset) == KeyResult::Accepted);
    CHECK(h.row(23).empty());
    CHECK(h.e.indicators().error == ErrorState::None);
    CHECK_FALSE(h.e.indicators().keyboard_locked);
}

TEST_CASE("tn5250 datastream: Clear Unit Alternate and its rejections") {
    Host wide(EngineOptions::for_terminal_type("IBM-3477-FG"));
    wide.write({0x04, 0x20, 0x00, 0x04, 0x11, 0x00, 0x00, 0x11, 0x1B, 0x84, 0xC1});
    CHECK(wide.out.empty());
    CHECK(wide.e.rows() == 27);
    CHECK(wide.e.cols() == 132);
    CHECK(wide.e.cell(26, 131).ch == 0xC1);
    wide.write({0x04, 0x20, 0x40});
    CHECK(sense_of(wide.take()) == kSenseInvalidClearAlt);
    wide.write({0x04, 0x40});
    CHECK(wide.e.cols() == 80);

    EngineOptions narrow;
    narrow.wide = false;
    Host n(narrow);
    n.write({0x04, 0x20, 0x00});
    const Record r = n.take();
    CHECK(r.flags == kFlagErr);
    CHECK(sense_of(r) == kSenseInvalidCommand);
    CHECK(n.e.cols() == 80);
}

TEST_CASE("tn5250 datastream: parameter errors are negative responses and skip CC1") {
    Host h;
    h.write({0x04, 0x40, 0x04, 0x11, 0x00, 0x08, 0x11, 0x00, 0x01});
    REQUIRE(h.out.size() == 1);
    CHECK(sense_of(h.take()) == kSenseRowCol);
    CHECK(h.e.indicators().keyboard_locked);  // the unlock in CC1 was not applied
    h.write({0x11, 0x01, 0x01});
    CHECK(sense_of(h.take()) == kSenseNoEscape);
    h.write({0x04, 0x99});
    CHECK(sense_of(h.take()) == kSenseInvalidCommand);
    h.write({0x04, 0x11, 0x00, 0x00, 0x11, 0x01, 0x05, 0x02, 0x01, 0x02, 0x40});  // RA backwards
    CHECK(sense_of(h.take()) == kSenseRaBackwards);
    h.write({0x04, 0x11, 0x00, 0x00, 0x11, 0x18, 0x50, 0x10, 0x00, 0x02, 0xC1, 0xC2});  // TD past the end
    CHECK(sense_of(h.take()) == kSenseWritePastEnd);
}

TEST_CASE("tn5250 datastream: Query reply (FR 15.27.2) claims only what the 3477 has") {
    Host fc(EngineOptions::for_terminal_type("IBM-3477-FC"));
    fc.write({0x04, 0xF3, 0x00, 0x05, 0xD9, 0x70, 0x00});
    REQUIRE(fc.out.size() == 1);
    const Record r = fc.take();
    CHECK(r.opcode == kOpNoOp);
    const Bytes& q = r.data;
    REQUIRE(q.size() == 3 + 0x44);
    CHECK(Bytes(q.begin(), q.begin() + 8) == Bytes{0x00, 0x00, 0x88, 0x00, 0x44, 0xD9, 0x70, 0x80});
    // Offsets as in RFC 1205 section 5 (counted from the cursor address).
    CHECK(Bytes(q.begin() + 8, q.begin() + 10) == Bytes{0x06, 0x00});  // "other 5250 emulator"
    CHECK(q[29] == 0x01);                // display station
    CHECK(Bytes(q.begin() + 30, q.begin() + 34) == Bytes{0xF3, 0xF4, 0xF7, 0xF7});
    CHECK(Bytes(q.begin() + 34, q.begin() + 37) == Bytes{0xF0, 0xC6, 0xC3});
    CHECK(q[37] == 0x02);                // keyboard ID
    CHECK(Bytes(q.begin() + 44, q.begin() + 46) == Bytes{0x01, 0x00});  // 256 input fields
    CHECK(q[49] == 0x3B);
    CHECK(q[50] == 0x31);                // 27x132 + 24x80 + colour
    for (size_t k = 51; k < q.size(); ++k) CHECK(q[k] == 0x00);  // no ext attrs, GUI, DBCS

    Host fg(EngineOptions::for_terminal_type("IBM-3477-FG"));
    fg.write({0x04, 0xF3, 0x00, 0x05, 0xD9, 0x70, 0x00});
    CHECK(fg.take().data[50] == 0x30);   // monochrome
    Host other(EngineOptions::for_terminal_type("IBM-3179-2"));
    other.write({0x04, 0xF3, 0x00, 0x05, 0xD9, 0x70, 0x00});
    CHECK(other.take().data[50] == 0x10);  // 24x80 only
}

TEST_CASE("tn5250 datastream: Read MDT / MDT Alternate / Input Fields with nulls and signed numeric") {
    auto setup = [](Host& h) {
        h.write({0x04, 0x40, 0x04, 0x11, 0x00, 0x08,
                 0x11, 0x03, 0x0A, 0x1D, 0x48, 0x00, 0x24, 0x00, 0x06, 0x40, 0xC1, 0x00, 0xC2,  // alpha, MDT on
                 0x11, 0x04, 0x0A, 0x1D, 0x4F, 0x00, 0x24, 0x00, 0x04, 0xF1, 0xF2, 0xF3, 0x60,  // signed numeric, -123
                 0x11, 0x05, 0x0A, 0x1D, 0x40, 0x00, 0x24, 0x00, 0x03});                     // untouched
    };
    {
        Host h;
        setup(h);
        h.write({0x04, 0x52, 0x00, 0x00});
        h.e.key_aid(Aid::Enter);
        CHECK(h.take().data == Bytes{0x03, 0x0B, 0xF1, 0x11, 0x03, 0x0B, 0x40, 0xC1, 0x40, 0xC2,
                                     0x11, 0x04, 0x0B, 0xF1, 0xF2, 0xD3});
    }
    {
        Host h;
        setup(h);
        h.write({0x04, 0x82, 0x00, 0x00});
        h.e.key_aid(Aid::F3);
        CHECK(h.take().data == Bytes{0x03, 0x0B, 0x33, 0x11, 0x03, 0x0B, 0x40, 0xC1, 0x00, 0xC2,
                                     0x11, 0x04, 0x0B, 0xF1, 0xF2, 0xD3});
    }
    {
        Host h;
        setup(h);
        h.write({0x04, 0x42, 0x00, 0x00});
        h.e.key_aid(Aid::Enter);
        CHECK(h.take().data == Bytes{0x03, 0x0B, 0xF1, 0x40, 0xC1, 0x40, 0xC2, 0x40, 0x40,
                                     0xF1, 0xF2, 0xD3, 0x40, 0x40, 0x40});
    }
    {
        Host h;
        setup(h);
        h.write({0x04, 0x52, 0x00, 0x00});
        h.e.key_aid(Aid::Clear);               // no data with Clear
        CHECK(h.take().data == Bytes{0x03, 0x0B, 0xBD});
    }
    {
        Host h;
        setup(h);
        h.write({0x04, 0x83});                 // Read Modified Immediate Alternate: AID 00
        CHECK(h.take().data == Bytes{0x03, 0x0B, 0x00, 0x11, 0x03, 0x0B, 0x40, 0xC1, 0x00, 0xC2,
                                     0x11, 0x04, 0x0B, 0xF1, 0xF2, 0xD3});
        h.write({0x04, 0x72});                 // Read Immediate: all fields, no SBA
        CHECK(h.take().data.size() == 3 + 6 + 3 + 3);
    }
}

TEST_CASE("tn5250 datastream: transparent field reads back with a TD prefix") {
    Host h;
    h.write({0x04, 0x40, 0x04, 0x11, 0x00, 0x08,
             0x11, 0x01, 0x01, 0x1D, 0x48, 0x00, 0x84, 0x00, 0x24, 0x00, 0x03,
             0x10, 0x00, 0x03, 0x00, 0x04, 0xFF});   // field contents need TD: 04 and FF
    REQUIRE(h.out.empty());
    h.write({0x04, 0x83});
    CHECK(h.take().data == Bytes{0x01, 0x02, 0x00, 0x11, 0x01, 0x02, 0x10, 0x00, 0x03, 0x00, 0x04, 0xFF});
}

TEST_CASE("tn5250 datastream: resequencing and the SOH command-key mask") {
    Host h;
    // SOH: resequence from field 2; PF1 returns no data. Field 2 chains to 1, 1 ends.
    h.write({0x04, 0x40, 0x04, 0x11, 0x00, 0x08,
             0x01, 0x07, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
             0x11, 0x01, 0x01, 0x1D, 0x48, 0x00, 0x80, 0xFF, 0x24, 0x00, 0x01, 0xC1,
             0x11, 0x02, 0x01, 0x1D, 0x48, 0x00, 0x80, 0x01, 0x24, 0x00, 0x01, 0xC2});
    h.write({0x04, 0x42, 0x00, 0x00});
    h.e.key_aid(Aid::F1);
    CHECK(h.take().data.size() == 3);       // masked: cursor and AID only
    h.write({0x04, 0x11, 0x00, 0x08, 0x04, 0x42, 0x00, 0x00});
    h.e.key_aid(Aid::Enter);
    const Bytes d = h.take().data;
    CHECK(Bytes(d.begin() + 3, d.end()) == Bytes{0xC2, 0xC1});
}

TEST_CASE("tn5250 datastream: CC0 nulls fields and resets MDT; CC1 alarm and message light") {
    Host h;
    h.write({0x04, 0x40, 0x04, 0x11, 0x00, 0x08,
             0x11, 0x01, 0x01, 0x1D, 0x48, 0x00, 0x24, 0x00, 0x02, 0xC1, 0xC2});
    REQUIRE(h.e.fields()[0].mdt());
    h.write({0x04, 0x11, 0xE0, 0x05});     // 111: reset all MDT, null non-bypass; alarm; MW on
    CHECK_FALSE(h.e.fields()[0].mdt());
    CHECK(h.e.cell(0, 1).ch == 0x00);
    CHECK(h.e.indicators().keyboard_locked);
    CHECK(h.e.take_alarms() == 1);
    CHECK(h.e.indicators().message_waiting);
    h.write({0x04, 0x11, 0x00, 0x02});
    CHECK_FALSE(h.e.indicators().message_waiting);
    Record on;
    on.opcode = kOpMessageOn;
    h.e.receive(on);
    CHECK(h.e.indicators().message_waiting);
    Record ci;
    ci.opcode = kOpCancelInvite;
    h.write({0x04, 0x52, 0x00, 0x00});
    h.e.receive(ci);
    REQUIRE(h.out.size() == 1);
    CHECK(h.take().opcode == kOpCancelInvite);
    CHECK_FALSE(h.e.read_pending());
}

TEST_CASE("tn5250 datastream: Roll moves lines and leaves vacated lines") {
    Host h;
    h.write(cat({{0x04, 0x40, 0x04, 0x11, 0x00, 0x00, 0x11, 0x01, 0x01}, eb("A"),
                 {0x11, 0x02, 0x01}, eb("B"), {0x11, 0x03, 0x01}, eb("C")}));
    h.write({0x04, 0x23, 0x01, 0x01, 0x03});
    CHECK(h.row(0) == "B");
    CHECK(h.row(1) == "C");
    CHECK(h.row(2) == "C");
    h.write({0x04, 0x23, 0x82, 0x01, 0x03});
    CHECK(h.row(2) == "B");
    CHECK(h.row(0) == "B");
    h.write({0x04, 0x23, 0x05, 0x01, 0x03});
    CHECK(sense_of(h.take()) == kSenseRollParam);
}

TEST_CASE("tn5250 datastream: Save Screen / Restore Screen round trip") {
    Host h;
    h.write(signon_stream());
    h.type("abc");
    ScreenSnapshot before;
    h.e.snapshot(before);
    const auto fields_before = h.e.fields();

    h.write({0x04, 0x02}, kOpSaveScreen);
    REQUIRE(h.out.size() == 1);
    const Record saved = h.take();
    CHECK(saved.opcode == kOpSaveScreen);
    REQUIRE(saved.data.size() > 2);
    CHECK(saved.data[0] == 0x04);
    CHECK(saved.data[1] == 0x12);
    CHECK(saved.data.size() < 600);         // run-length coded

    h.write({0x04, 0x40, 0x04, 0x11, 0x00, 0x00, 0x11, 0x01, 0x01, 0xC1});
    CHECK(h.e.fields().empty());
    h.write(saved.data, kOpRestoreScreen);
    CHECK(h.out.empty());
    ScreenSnapshot after;
    h.e.snapshot(after);
    CHECK(after.rows == before.rows);
    CHECK(after.cols == before.cols);
    CHECK(after.cells == before.cells);
    CHECK(after.cursor_row == before.cursor_row);
    CHECK(after.cursor_col == before.cursor_col);
    CHECK(after.indicators.keyboard_locked == before.indicators.keyboard_locked);
    REQUIRE(h.e.fields().size() == fields_before.size());
    for (size_t k = 0; k < fields_before.size(); ++k) {
        CHECK(h.e.fields()[k].start == fields_before[k].start);
        CHECK(h.e.fields()[k].ffw == fields_before[k].ffw);
    }
    CHECK(h.e.read_pending());              // the outstanding read is part of the image

    h.write({0x04, 0x12, 'X', 'Y'}, kOpRestoreScreen);
    CHECK(sense_of(h.take()) == kSenseRestoreData);
}

TEST_CASE("tn5250 datastream: Read Screen returns the buffer raw") {
    Host h;
    h.write(signon_stream());
    h.write({0x04, 0x62}, kOpReadScreen);
    const Record r = h.take();
    REQUIRE(r.data.size() == 24 * 80);
    CHECK(r.data[5 * 80 + 51] == 0x24);
    CHECK(r.data[0] == 0x00);
}

namespace {
// An input field (FFW 4000, attribute 24) of 5 positions whose leading
// attribute is at row r, column c.
Bytes field5(uint8_t r, uint8_t c) { return {0x11, r, c, 0x1D, 0x40, 0x00, 0x24, 0x00, 0x05}; }
const Bytes kClearAndWrite{0x04, 0x40, 0x04, 0x11, 0x00, 0x08};
const Bytes kReadMdt{0x04, 0x52, 0x00, 0x00};
} // namespace

TEST_CASE("tn5250 datastream: input fields one attribute apart are all kept") {
    // Data at row 5 columns 11-15 and 17-21: column 16 is the ending
    // attribute of the first and the leading attribute of the second.
    for (bool reversed : {false, true}) {
        CAPTURE(reversed);
        Host h;
        h.write(reversed ? cat({kClearAndWrite, field5(7, 10), field5(5, 16), field5(5, 10), kReadMdt})
                         : cat({kClearAndWrite, field5(5, 10), field5(5, 16), field5(7, 10), kReadMdt}));
        CHECK(h.out.empty());
        REQUIRE(h.e.fields().size() == 3);
        CHECK(h.e.fields()[0].start == 4 * 80 + 10);
        CHECK(h.e.fields()[1].start == 4 * 80 + 16);
        CHECK(h.e.fields()[2].start == 6 * 80 + 10);
        for (const Field& f : h.e.fields()) CHECK(f.length == 5);
        CHECK(h.e.cell(4, 9).ch == 0x24);
        CHECK(h.e.cell(4, 15).ch == 0x24);   // shared: still the second field's attribute
        CHECK(h.e.cell(4, 21).ch == 0x20);
        CHECK(h.e.cell(6, 15).ch == 0x20);

        h.type("abcde");                     // fills the first field, moves to the second
        h.type("fg");
        CHECK(h.e.key_aid(Aid::Enter) == KeyResult::Accepted);
        REQUIRE(h.out.size() == 1);
        CHECK(h.take().data == cat({{0x05, 0x13, 0xF1},
                                    {0x11, 0x05, 0x0B}, eb("abcde"),
                                    {0x11, 0x05, 0x11}, eb("fg")}));
    }
}

TEST_CASE("tn5250 datastream: a field that really overlaps another replaces it") {
    Host h;
    h.write(cat({kClearAndWrite, field5(5, 10), field5(5, 16)}));
    REQUIRE(h.e.fields().size() == 2);
    // Data at columns 5-9: its ending attribute is the first field's leading one.
    h.write(cat({{0x04, 0x11, 0x00, 0x00}, field5(5, 4)}));
    REQUIRE(h.e.fields().size() == 3);
    CHECK(h.e.cell(4, 9).ch == 0x24);
    // Data at columns 6-10 covers that leading attribute: both neighbours go.
    h.write(cat({{0x04, 0x11, 0x00, 0x00}, field5(5, 5)}));
    REQUIRE(h.e.fields().size() == 2);
    CHECK(h.e.fields()[0].start == 4 * 80 + 5);
    CHECK(h.e.fields()[1].start == 4 * 80 + 16);
    // A leading attribute on another field's last data position.
    h.write(cat({{0x04, 0x11, 0x00, 0x00}, field5(5, 21)}));
    REQUIRE(h.e.fields().size() == 2);
    CHECK(h.e.fields()[1].start == 4 * 80 + 21);
    CHECK(h.out.empty());
}

TEST_CASE("tn5250 datastream: a redefined field drops the fields its new length covers") {
    Host h;
    h.write(cat({kClearAndWrite, field5(5, 10), field5(5, 16), field5(7, 10)}));
    REQUIRE(h.e.fields().size() == 3);
    h.write(cat({{0x04, 0x11, 0x00, 0x00}, field5(5, 10)}));   // same extent: nothing else changes
    REQUIRE(h.e.fields().size() == 3);
    h.write({0x04, 0x11, 0x00, 0x00, 0x11, 0x05, 0x0A, 0x1D, 0x40, 0x00, 0x24, 0x00, 0x08});
    REQUIRE(h.e.fields().size() == 2);
    CHECK(h.e.fields()[0].start == 4 * 80 + 10);
    CHECK(h.e.fields()[0].length == 8);
    CHECK(h.e.fields()[1].start == 6 * 80 + 10);
    CHECK(h.out.empty());
}

TEST_CASE("tn5250 datastream: one record gets at most a few replies") {
    Host h;
    h.write(signon_stream());
    Bytes many;
    for (int k = 0; k < 1000; ++k) many.insert(many.end(), {0x04, 0x62});
    h.write(many, kOpReadScreen);
    REQUIRE(h.out.size() == 5);              // four screens, then the negative response
    for (int k = 0; k < 4; ++k) CHECK(h.take().data.size() == 24 * 80);
    const Record neg = h.take();
    CHECK((neg.flags & kFlagErr) != 0);
    CHECK(sense_of(neg) == kSenseInvalidCommand);

    // Every reply-generating command counts, and each record starts afresh.
    Bytes mixed{0x04, 0x72, 0x04, 0x83, 0x04, 0x02, 0x04, 0xF3};
    for (int k = 0; k < 10; ++k) mixed.insert(mixed.end(), {0x00, 0x05, 0xD9, 0x70, 0x00});
    h.write(mixed);
    REQUIRE(h.out.size() == 5);
    CHECK(sense_of(h.out.back()) == kSenseInvalidCommand);
    h.out.clear();
    h.write({0x04, 0xF3, 0x00, 0x05, 0xD9, 0x70, 0x00});
    REQUIRE(h.out.size() == 1);
    CHECK(h.take().data == h.e.query_reply());
}

TEST_CASE("tn5250 datastream: repeat orders are limited per record, generously") {
    auto repeats = [](int n) {
        Bytes d{0x04, 0x40, 0x04, 0x11, 0x00, 0x00};
        for (int k = 0; k < n; ++k)
            d.insert(d.end(), {0x11, 0x01, 0x01, 0x02, 0x18, 0x50, uint8_t(k & 1 ? 0x5C : 0x40)});
        return d;
    };
    Host h;
    h.write(repeats(100));                   // a hundred whole-screen RAs: far beyond a real screen
    CHECK(h.out.empty());
    CHECK(h.e.cell(23, 79).ch == 0x5C);
    h.write(repeats(1000));
    REQUIRE(h.out.size() == 1);
    CHECK(sense_of(h.take()) == kSenseInvalidCommand);
    h.write(repeats(1));                     // the next record is unaffected
    CHECK(h.out.empty());
    CHECK(h.e.cell(23, 79).ch == 0x40);
}

TEST_CASE("tn5250 datastream: FCWs per field are limited and the largest save image fits its lengths") {
    auto field = [](uint8_t r, uint8_t c, size_t fcws) {
        Bytes sf{0x11, r, c, 0x1D, 0x40, 0x00};
        for (size_t k = 0; k < fcws; ++k) sf.insert(sf.end(), {0x89, 0x01});
        sf.insert(sf.end(), {0x24, 0x00, 0x01});
        return sf;
    };
    Host h;
    h.write(cat({kClearAndWrite, field(1, 1, kMaxFcws + 1)}));
    REQUIRE(h.out.size() == 1);
    CHECK(sense_of(h.take()) == kSenseFormatOverflow);
    CHECK(h.e.fields().empty());

    // 256 fields (the format table's size), each with every FCW allowed.
    Bytes d = kClearAndWrite;
    for (int k = 0; k < 256; ++k) {
        const Bytes sf = field(uint8_t(1 + k / 32), uint8_t(1 + 2 * (k % 32)), kMaxFcws);
        d.insert(d.end(), sf.begin(), sf.end());
    }
    h.write(d);
    CHECK(h.out.empty());
    REQUIRE(h.e.fields().size() == 256);
    CHECK(h.e.fields()[255].fcws.size() == kMaxFcws);

    h.write({0x04, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}, kOpSaveScreen);
    REQUIRE(h.out.size() == 1);
    const Record saved = h.take();
    CHECK(saved.opcode == kOpSaveScreen);
    REQUIRE(saved.data.size() > 11);
    CHECK(saved.data.size() < 0x8000);
    const size_t ll = (size_t(saved.data[2]) << 8) | saved.data[3];
    CHECK(ll == saved.data.size() - 2);                           // 04 13 LL: LL counts itself
    const size_t image = (size_t(saved.data[9]) << 8) | saved.data[10];
    CHECK(image == saved.data.size() - 4);                        // the image's own length
    h.write({0x04, 0x40});
    h.write(saved.data, kOpRestoreScreen);
    CHECK(h.out.empty());
    REQUIRE(h.e.fields().size() == 256);
    CHECK(h.e.fields()[255].fcws.size() == kMaxFcws);
}
