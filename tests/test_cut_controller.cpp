// SPDX-License-Identifier: GPL-3.0-or-later
// The coax CUT controller (host/coax/cut_controller.hpp): key bytes from
// the terminal's poll responses.
#include "host/coax/cut_controller.hpp"
#include "tn3270/engine.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <functional>
#include <initializer_list>
#include <thread>

using namespace em34xx;

TEST_CASE("3483 on coax: set-3 key bytes with F0 breaks, Shift releases Caps Lock") {
    // The 3483 forwards its keyboard's bytes: Shift 12 ... F0 12, Caps
    // Lock 14 ... F0 14 [observed]. Typing after a released Shift or a
    // Caps Lock pressed twice must give lower case.
    using K = CoaxKey::Kind;
    CutController cut(nullptr);
    CoaxKey a;
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
    CHECK(type({0x53}) == 0xE0);                          // \ left of New Line
}

TEST_CASE("3483 on coax: the 122-key keyboard's 3270 keys") {
    // PCOMM/3270's 122-key layout (S20H-1773 5.1.1) by set-3 position.
    using K = CoaxKey::Kind;
    using tn3270::Aid;
    using tn3270::Key;
    CutController cut(nullptr);
    // Presses `pos` with Shift (12) or Alt (19) held as asked.
    auto press = [&](uint8_t pos, bool shift = false, bool alt = false) {
        CoaxKey k, out;
        if (shift) cut.decode_key(0x12, k);
        if (alt) cut.decode_key(0x19, k);
        cut.decode_key(pos, out);
        if (alt) { cut.decode_key(0xF0, k); cut.decode_key(0x19, k); }
        if (shift) { cut.decode_key(0xF0, k); cut.decode_key(0x12, k); }
        return out;
    };
    auto is_aid = [](const CoaxKey& k, uint8_t aid) { return k.kind == K::Aid && k.aid == aid; };
    auto is_key = [](const CoaxKey& k, Key key) { return k.kind == K::Key && k.key == key; };
    CHECK(press(0x05).kind == K::Attn);
    CHECK(press(0x05, false, true).kind == K::SysReq);
    CHECK(press(0x05, true).kind == K::SysReq);
    CHECK(is_aid(press(0x06), uint8_t(Aid::Clear)));
    CHECK(is_key(press(0x0C), Key::EraseInput));
    CHECK(is_key(press(0x0B), Key::EraseEOF));
    CHECK(is_aid(press(0x67), uint8_t(Aid::PA1)));
    CHECK(is_key(press(0x67, true), Key::Dup));
    CHECK(is_aid(press(0x6E), uint8_t(Aid::PA2)));
    CHECK(is_key(press(0x6E, true), Key::FieldMark));
    CHECK(is_aid(press(0x6F, false, true), uint8_t(Aid::PA3)));
    CHECK(press(0x6F).kind == K::None);
    CHECK(is_key(press(0x64), Key::BackTab));
    CHECK(is_key(press(0x65), Key::Insert));
    CHECK(is_key(press(0x6D), Key::Delete));
    CHECK(is_key(press(0x62, false, true), Key::Home));
    CHECK(press(0x62).kind == K::None);   // Rule: local
    CHECK(is_key(press(0x0D, true), Key::BackTab));
    CHECK(is_key(press(0x5A), Key::NewLine));
    CHECK(is_aid(press(0x58), uint8_t(Aid::Enter)));
    CHECK(is_key(press(0x11), Key::Reset));
    CHECK(is_aid(press(0x07), tn3270::pf_aid(1)));
    CHECK(is_aid(press(0x5E), tn3270::pf_aid(12)));
    CHECK(is_aid(press(0x08), tn3270::pf_aid(13)));   // the upper row
    CHECK(is_aid(press(0x5F), tn3270::pf_aid(24)));
    CHECK(press(0x83).kind == K::None);
}

TEST_CASE("CUT controller: a terminal is reset only after two seconds of answers") {
    // A terminal that has just come on line is still starting up; a RESET
    // then left the 3472's keyboard disabled [observed in emulation].
    CutController cut(nullptr);
    uint64_t t = 0;
    // One poll: the controller's next message, answered with TT/AR (or
    // nothing); returns that message's first word.
    auto poll = [&](bool answer) {
        t += 4'000'000;
        cut.advance(t);
        CoaxMessage msg;
        if (!cut.to_terminal(msg)) return CoaxWord{0xFFFF};
        if (answer) cut.from_terminal({CoaxWord::turnaround()});
        else cut.no_response();
        return msg.front();
    };
    auto sent_reset = [&](unsigned polls, bool answer) {
        for (unsigned i = 0; i < polls; ++i)
            if (poll(answer).bits == CoaxWord::command(0x02).bits) return true;
        return false;
    };
    CHECK_FALSE(sent_reset(100, false));   // nobody there
    CHECK_FALSE(sent_reset(400, true));    // 1.6 s of answers: still waiting
    CHECK_FALSE(sent_reset(3, false));     // silent again: start over
    CHECK_FALSE(sent_reset(400, true));
    CHECK(sent_reset(200, true));          // past 2 s: RESET
}

TEST_CASE("CUT controller: feature 79 is an EAB with colour, 78 one without") {
    // The 3483 answers 79 at feature address 7; the 3472 answers 78 for its
    // display type 1 [observed in emulation]. Identify runs again for a
    // terminal that replaces another.
    CutController cut(nullptr);
    uint64_t t = 0;
    // Answers the controller's messages as a terminal with `eab` at feature
    // address 7 would, until it is running.
    auto identify = [&](uint8_t eab) {
        for (int i = 0; i < 2000; ++i) {
            t += 4'000'000;
            cut.advance(t);
            CoaxMessage msg;
            if (!cut.to_terminal(msg)) continue;
            const CoaxWord w = msg.front();
            const uint8_t c = w.byte();
            if (w.bits == CoaxWord::command(0x01).bits) cut.from_terminal({CoaxWord::turnaround()});
            else if (w.bits == CoaxWord::command(0x02).bits) {
                cut.from_terminal({CoaxWord::turnaround()});
                // The next poll gets the power-on-reset status.
                t += 4'000'000;
                cut.advance(t);
                CoaxMessage p;
                if (cut.to_terminal(p)) cut.from_terminal({CoaxWord{uint16_t(0x02 << 2 | 2)}});
            } else if (w.bits == CoaxWord::command(0x09).bits) cut.from_terminal({CoaxWord::data(0x04)});
            else if ((c & 0x0F) == 0x07 && w.is_command())
                cut.from_terminal({CoaxWord::data((c >> 4) == 7 ? eab : 0x00)});
            else {
                cut.from_terminal({CoaxWord::turnaround()});
                if (w.bits == CoaxWord::command(0x0A).bits) return true;   // load control: running
            }
        }
        return false;
    };
    REQUIRE(identify(0x79));
    CHECK(cut.has_eab());
    CHECK(cut.has_eab_colour());
    cut.terminal_reset();
    REQUIRE(identify(0x78));
    CHECK(cut.has_eab());
    CHECK_FALSE(cut.has_eab_colour());
    cut.terminal_reset();
    REQUIRE(identify(0x00));
    CHECK_FALSE(cut.has_eab());
}

namespace {
// A host that accepts the connection and closes it at once.
class ClosingTransport : public tn5250::Transport {
public:
    bool connect(const std::string&, uint16_t, std::string&) override { return true; }
    int recv(uint8_t*, size_t, int) override { return -1; }
    bool send(const uint8_t*, size_t) override { return true; }
    void close() override {}
    std::string describe() const override { return "closing"; }
};
} // namespace

TEST_CASE("CUT controller: a session the host closes is opened again; a hold lasts until reset") {
    Tn5250Config cfg;
    cfg.host = "host.example";
    cfg.tls = true;
    CutController::ClientFactory make = [cfg] {
        return std::make_unique<tn3270::Client>(
            cfg, [](std::string&) { return std::unique_ptr<tn5250::Transport>(new ClosingTransport); });
    };
    SessionCycleOptions opt;
    opt.reconnect_min_ms = 1;
    opt.reconnect_max_ms = 1;
    CutController cut(make, cfg, opt);
    CHECK(cut.bridges_sessions());
    auto run_until = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 400 && !done(); ++i) {
            cut.advance(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    };
    CHECK(run_until([&] { return cut.sessions_started() >= 3; }));   // redialled
    auto st = cut.session_status();
    REQUIRE(st.size() == 1);
    CHECK(st[0].tls);
    CHECK(st[0].peer == "host.example:23");
    CHECK(cut.disconnect_station(0));                // a refused certificate
    const unsigned held_at = cut.sessions_started();
    run_until([] { return false; });
    CHECK(cut.sessions_started() == held_at);
    CHECK(cut.session_status()[0].held);
    cut.terminal_reset();                            // lifts the hold
    CHECK(run_until([&] { return cut.sessions_started() > held_at; }));
}
