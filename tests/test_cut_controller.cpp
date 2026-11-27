// SPDX-License-Identifier: GPL-3.0-or-later
// The coax CUT controller (host/coax/cut_controller.hpp): key bytes from
// the terminal's poll responses.
#include "host/coax/cut_controller.hpp"

#include <doctest/doctest.h>

#include <initializer_list>

using namespace em34xx;

TEST_CASE("3483 on coax: set-3 key bytes with F0 breaks, Shift releases Caps Lock") {
    // The 3483 forwards its keyboard's bytes: Shift 12 ... F0 12, Caps
    // Lock 14 ... F0 14 [observed]. Typing after a released Shift or a
    // Caps Lock pressed twice must give lower case.
    using K = wsc::OperatorAction::Kind;
    CutController cut(nullptr);
    wsc::OperatorAction a;
    auto type = [&](std::initializer_list<uint8_t> bytes) {
        uint8_t ch = 0;
        for (uint8_t b : bytes)
            if (cut.decode_key(b, a) && a.kind == K::Character) ch = a.ebcdic;
        return ch;
    };
    CHECK(type({0x1C}) == 0x81);                          // a
    CHECK(type({0x12, 0x1C, 0xF0, 0x12}) == 0xC1);        // Shift+a: A
    CHECK_FALSE(cut.keyboard().shift());
    CHECK(type({0x1C}) == 0x81);                          // a again
    CHECK(type({0x14, 0xF0, 0x14, 0x1C}) == 0xC1);        // Caps Lock: A
    CHECK(cut.keyboard().caps_lock());
    CHECK(type({0x14, 0xF0, 0x14, 0x1C}) == 0x81);        // Caps Lock again: a
    CHECK(type({0x14, 0xF0, 0x14}) == 0);
    CHECK(type({0x12, 0xF0, 0x12, 0x1C}) == 0x81);        // Shift releases the lock
    CHECK_FALSE(cut.keyboard().caps_lock());
    CHECK(type({0x59, 0x1E}) == 0x7C);                    // right Shift+2: @
    CHECK(type({0xF0, 0x59, 0x1E}) == 0xF2);              // released: 2
}
