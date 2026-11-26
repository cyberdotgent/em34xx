// SPDX-License-Identifier: GPL-3.0-or-later
// The 3270 data stream engine against records built from the 3270 Data
// Stream Programmer's Reference (GA23-0059).
#include "tn3270/engine.hpp"

#include <doctest/doctest.h>

using namespace em34xx::tn3270;

namespace {

std::vector<uint8_t> addr(int a) {
    uint8_t b1, b2;
    encode_address(a, b1, b2);
    return {b1, b2};
}

std::vector<std::vector<uint8_t>> run(Engine& e, std::vector<uint8_t> rec) {
    std::vector<std::vector<uint8_t>> out;
    e.process(rec, out);
    return out;
}

} // namespace

TEST_CASE("3270 buffer addresses: 12-bit code and 14-bit binary") {
    uint8_t b1, b2;
    encode_address(0, b1, b2);
    CHECK((b1 == 0x40 && b2 == 0x40));
    encode_address(80, b1, b2);   // row 1, column 0: 1*64+16
    CHECK((b1 == 0xC1 && b2 == 0x50));
    CHECK(decode_address(0xC1, 0x50) == 80);
    CHECK(decode_address(0x0F, 0xA0) == 0x0FA0);   // 14-bit form
    for (int a : {0, 1, 79, 1919, 3563}) {
        encode_address(a, b1, b2);
        CHECK(decode_address(b1, b2) == a);
    }
}

TEST_CASE("Erase/Write with fields, insert cursor and keyboard restore") {
    Engine e;
    // EW, WCC restore|reset MDT, SBA(0) SF(protected) "Hi", SBA(10) SF(unprotected) IC
    std::vector<uint8_t> rec{0xF5, 0xC3, 0x11};
    for (uint8_t b : addr(0)) rec.push_back(b);
    rec.insert(rec.end(), {0x1D, 0x60, 0xC8, 0x89, 0x11});
    for (uint8_t b : addr(10)) rec.push_back(b);
    rec.insert(rec.end(), {0x1D, 0x40, 0x13});
    run(e, rec);
    CHECK(e.formatted());
    CHECK(e.cell(0).fa);
    CHECK(fa_protected(e.cell(0).ch));
    CHECK(e.cell(1).ch == 0xC8);   // 'H'
    CHECK(e.cell(2).ch == 0x89);   // 'i'
    CHECK(e.cell(10).fa);
    CHECK(e.cursor() == 11);
    CHECK_FALSE(e.keyboard_locked());
    // Typing into the unprotected field sets its MDT; a protected one refuses.
    CHECK(e.type(0xC1));
    CHECK(fa_modified(e.cell(10).ch));
    CHECK(e.cursor() == 12);
}

TEST_CASE("Enter reads the modified fields with nulls suppressed") {
    Engine e;
    std::vector<uint8_t> rec{0xF5, 0x02, 0x1D, 0x60, 0xC1, 0x1D, 0x40};
    run(e, rec);   // FA(prot) 'A' FA(unprot) at 0..2; input field from 3
    std::vector<std::vector<uint8_t>> out;
    e.key(Key::Home);
    CHECK(e.cursor() == 3);
    e.type(0xF1);
    e.type(0xF2);
    e.aid(uint8_t(Aid::Enter), out);
    REQUIRE(out.size() == 1);
    const auto& r = out[0];
    REQUIRE(r.size() >= 8);
    CHECK(r[0] == 0x7D);
    CHECK(decode_address(r[1], r[2]) == 5);   // cursor after two characters
    CHECK(r[3] == 0x11);
    CHECK(decode_address(r[4], r[5]) == 3);
    CHECK(r[6] == 0xF1);
    CHECK(r[7] == 0xF2);
    CHECK(r.size() == 8);
    CHECK(e.keyboard_locked());
}

TEST_CASE("PA and Clear are short reads; Clear erases") {
    Engine e;
    run(e, {0xF5, 0x02, 0xC1});
    std::vector<std::vector<uint8_t>> out;
    e.aid(uint8_t(Aid::PA1), out);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == std::vector<uint8_t>{0x6C});
    e.key(Key::Reset);
    out.clear();
    e.aid(uint8_t(Aid::Clear), out);
    CHECK(out[0] == std::vector<uint8_t>{0x6D});
    CHECK(e.cell(0).ch == 0);
}

TEST_CASE("Repeat to Address, Erase Unprotected to Address, GE") {
    Engine e;
    std::vector<uint8_t> rec{0xF5, 0x02, 0x3C};
    for (uint8_t b : addr(5)) rec.push_back(b);
    rec.push_back(0x5C);   // '*' x5
    rec.insert(rec.end(), {0x08, 0xAD});   // GE: APL character
    run(e, rec);
    for (int a = 0; a < 5; ++a) CHECK(e.cell(a).ch == 0x5C);
    CHECK(e.cell(5).ch == 0xAD);
    CHECK(e.cell(5).charset == 0xF1);
    std::vector<uint8_t> eua{0xF1, 0x02, 0x11};
    for (uint8_t b : addr(0)) eua.push_back(b);
    eua.push_back(0x12);
    for (uint8_t b : addr(3)) eua.push_back(b);
    run(e, eua);   // unformatted: everything is unprotected
    CHECK(e.cell(0).ch == 0);
    CHECK(e.cell(2).ch == 0);
    CHECK(e.cell(3).ch == 0x5C);
}

TEST_CASE("SFE and SA carry colour and highlighting") {
    Engine e;
    run(e, {0xF5, 0x02, 0x29, 0x03, 0xC0, 0x60, 0x42, 0xF2, 0x41, 0xF4, 0x28, 0x42, 0xF6, 0xC1});
    CHECK(e.cell(0).fa);
    CHECK(e.cell(0).colour == 0xF2);
    CHECK(e.cell(0).highlight == 0xF4);
    CHECK(e.cell(1).ch == 0xC1);
    CHECK(e.cell(1).colour == 0xF6);
}

TEST_CASE("Read Partition Query answers with a Query Reply") {
    Engine e;
    auto out = run(e, {0xF3, 0x00, 0x05, 0x01, 0xFF, 0x02});
    REQUIRE(out.size() == 1);
    CHECK(out[0][0] == 0x88);
    CHECK(out[0][3] == 0x81);
    CHECK(out[0][4] == 0x80);   // Summary first
}

TEST_CASE("Erase All Unprotected clears input and unlocks") {
    Engine e;
    run(e, {0xF5, 0x02, 0x1D, 0x60, 0xC1, 0x1D, 0x40, 0xC2, 0xC3});
    std::vector<std::vector<uint8_t>> out;
    e.aid(uint8_t(Aid::Enter), out);
    CHECK(e.keyboard_locked());
    run(e, {0x6F});
    CHECK_FALSE(e.keyboard_locked());
    CHECK(e.cell(3).ch == 0);
    CHECK(e.cell(1).ch == 0xC1);
    CHECK(e.cursor() == 3);
}

#include "tn3270/display_code.hpp"

TEST_CASE("EBCDIC to the 3483's display code") {
    CHECK(ebcdic_to_display(0xC1) == 0xA0);   // A
    CHECK(ebcdic_to_display(0x81) == 0x80);   // a
    CHECK(ebcdic_to_display(0xF0) == 0x20);   // 0
    CHECK(ebcdic_to_display(0x40) == 0x10);   // space
    CHECK(ebcdic_to_display(0x7E) == 0x11);   // =
    CHECK(ebcdic_to_display(0x5E) == 0xBE);   // ;
    CHECK(ebcdic_to_display(0x00) == 0x00);
    CHECK(display_field_attribute(0x20) == 0xE0);
}
