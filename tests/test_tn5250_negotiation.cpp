// SPDX-License-Identifier: GPL-3.0-or-later
// TN5250 telnet negotiation and record framing (RFC 854/856/885/1091/
// 1205/1572/4777). The transcripts are written by hand from the RFC
// examples and option definitions.
#include "tn5250/client.hpp"
#include "tn5250/record.hpp"
#include "tn5250/telnet.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace tn34xx;
using namespace tn34xx::tn5250;
using namespace tn34xx::tn5250::telnet;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes str(const std::string& s) { return Bytes(s.begin(), s.end()); }

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

struct Harness {
    TelnetCodec codec;
    std::vector<Bytes> records;
    explicit Harness(TelnetSettings s) : codec(std::move(s)) {}
    Bytes feed(const Bytes& in) {
        Bytes reply;
        codec.receive(in.data(), in.size(), reply, records);
        return reply;
    }
};

TelnetSettings settings_3477() {
    Tn5250Config cfg;
    cfg.host = "example";
    cfg.device_name = "TN34XX01";
    return telnet_settings_for(cfg);
}

} // namespace

TEST_CASE("tn5250 negotiation: iSeries-style bundled transcript (RFC 4777 section 3)") {
    Harness h(settings_3477());
    // Server bundles DO NEW-ENVIRON and DO TERMINAL-TYPE.
    CHECK(h.feed({IAC, DO, OPT_NEW_ENVIRON, IAC, DO, OPT_TTYPE}) ==
          Bytes{IAC, WILL, OPT_NEW_ENVIRON, IAC, WILL, OPT_TTYPE});
    // SEND VAR USERVAR: everything we have.
    const Bytes env = h.feed({IAC, SB, OPT_NEW_ENVIRON, ENV_SEND, ENV_VAR, ENV_USERVAR, IAC, SE});
    CHECK(env == cat({{IAC, SB, OPT_NEW_ENVIRON, ENV_IS},
                      {ENV_USERVAR}, str("DEVNAME"), {ENV_VALUE}, str("TN34XX01"),
                      {ENV_USERVAR}, str("KBDTYPE"), {ENV_VALUE}, str("USB"),
                      {ENV_USERVAR}, str("CODEPAGE"), {ENV_VALUE}, str("37"),
                      {ENV_USERVAR}, str("CHARSET"), {ENV_VALUE}, str("697"),
                      {IAC, SE}}));
    CHECK(h.feed({IAC, SB, OPT_TTYPE, TT_SEND, IAC, SE}) ==
          cat({{IAC, SB, OPT_TTYPE, TT_IS}, str("IBM-3477-FC"), {IAC, SE}}));
    CHECK_FALSE(h.codec.transparent());
    CHECK(h.feed({IAC, DO, OPT_EOR}) == Bytes{IAC, WILL, OPT_EOR});
    CHECK(h.feed({IAC, WILL, OPT_EOR}) == Bytes{IAC, DO, OPT_EOR});
    CHECK(h.feed({IAC, DO, OPT_BINARY}) == Bytes{IAC, WILL, OPT_BINARY});
    CHECK_FALSE(h.codec.transparent());
    CHECK(h.feed({IAC, WILL, OPT_BINARY}) == Bytes{IAC, DO, OPT_BINARY});
    CHECK(h.codec.transparent());
    // Log has both directions.
    const auto& log = h.codec.log();
    REQUIRE(log.size() >= 4);
    CHECK(log[0] == "recv DO NEW-ENVIRON");
    CHECK(log[1] == "send WILL NEW-ENVIRON");
}

TEST_CASE("tn5250 negotiation: RFC 1205 order and no loops") {
    Harness h(TelnetSettings{});
    CHECK(h.feed({IAC, DO, OPT_TTYPE}) == Bytes{IAC, WILL, OPT_TTYPE});
    // A repeated request for an enabled option is not answered (RFC 854).
    CHECK(h.feed({IAC, DO, OPT_TTYPE}).empty());
    CHECK(h.feed({IAC, WILL, OPT_EOR}) == Bytes{IAC, DO, OPT_EOR});
    CHECK(h.feed({IAC, WILL, OPT_EOR}).empty());
    // Unsupported options are refused once per request.
    CHECK(h.feed({IAC, DO, OPT_NAWS}) == Bytes{IAC, WONT, OPT_NAWS});
    CHECK(h.feed({IAC, WILL, OPT_ECHO}) == Bytes{IAC, DONT, OPT_ECHO});
    // Disabling an enabled option is acknowledged; disabling a disabled one is not.
    CHECK(h.feed({IAC, DONT, OPT_TTYPE}) == Bytes{IAC, WONT, OPT_TTYPE});
    CHECK(h.feed({IAC, DONT, OPT_TTYPE}).empty());
    CHECK(h.feed({IAC, WONT, OPT_EOR}) == Bytes{IAC, DONT, OPT_EOR});
    // TIMING-MARK is acknowledged without becoming enabled (RFC 860).
    CHECK(h.feed({IAC, DO, OPT_TIMING_MARK}) == Bytes{IAC, WILL, OPT_TIMING_MARK});
    CHECK_FALSE(h.codec.local_enabled(OPT_TIMING_MARK));
    // Subnegotiation for a disabled option is ignored.
    CHECK(h.feed({IAC, SB, OPT_TTYPE, TT_SEND, IAC, SE}).empty());
}

TEST_CASE("tn5250 negotiation: IBMRSEED request is recorded, no password or USER is sent") {
    Harness h(settings_3477());
    h.feed({IAC, DO, OPT_NEW_ENVIRON});
    // Seed bytes include 0x01 and 0x02, which must arrive ESC-quoted, and
    // 0xFF, doubled as IAC IAC.
    const Bytes req = cat({{IAC, SB, OPT_NEW_ENVIRON, ENV_SEND, ENV_USERVAR}, str("IBMRSEED"),
                           {0x7D, ENV_ESC, 0x01, ENV_ESC, 0x02, 0x48, IAC, IAC, 0x18, 0x08, 0x04},
                           {ENV_USERVAR}, str("IBMSUBSPW"), {ENV_VAR, ENV_USERVAR, IAC, SE}});
    const Bytes reply = h.feed(req);
    CHECK(h.codec.server_seed() == Bytes{0x7D, 0x01, 0x02, 0x48, 0xFF, 0x18, 0x08, 0x04});
    const std::string text(reply.begin(), reply.end());
    CHECK(text.find("IBMSUBSPW") == std::string::npos);
    CHECK(text.find("IBMRSEED") == std::string::npos);
    CHECK(text.find("USER\x01") == std::string::npos);
    CHECK(text.find("DEVNAME") != std::string::npos);
}

TEST_CASE("tn5250 negotiation: SEND for one variable answers only that one") {
    Harness h(settings_3477());
    h.feed({IAC, DO, OPT_NEW_ENVIRON});
    const Bytes reply =
        h.feed(cat({{IAC, SB, OPT_NEW_ENVIRON, ENV_SEND, ENV_USERVAR}, str("DEVNAME"), {IAC, SE}}));
    CHECK(reply == cat({{IAC, SB, OPT_NEW_ENVIRON, ENV_IS, ENV_USERVAR}, str("DEVNAME"), {ENV_VALUE},
                        str("TN34XX01"), {IAC, SE}}));
}

TEST_CASE("tn5250 settings: DEVNAME only when configured; KBDTYPE only when known") {
    Tn5250Config cfg;
    cfg.host = "h";
    auto names = [](const TelnetSettings& s) {
        std::string n;
        for (const auto& v : s.environment) n += (v.user ? "U:" : "V:") + v.name + " ";
        return n;
    };
    CHECK(names(telnet_settings_for(cfg)) == "U:KBDTYPE U:CODEPAGE U:CHARSET ");
    cfg.codepage = "500";
    CHECK(names(telnet_settings_for(cfg)).empty());
    cfg.env["KBDTYPE"] = "BLI";
    cfg.env["JOB"] = "X";
    cfg.env["IBMSENDCONFREC"] = "NO";
    CHECK(names(telnet_settings_for(cfg)) == "U:KBDTYPE U:CODEPAGE U:IBMSENDCONFREC V:JOB ");
}

TEST_CASE("tn5250 environment values with VAR/VALUE/ESC/USERVAR bytes are ESC-quoted") {
    TelnetSettings s;
    s.environment.push_back(EnvVariable{"X", {0x00, 0x01, 0x41, 0x02, 0x03, 0xFF}, true});
    Harness h(s);
    h.feed({IAC, DO, OPT_NEW_ENVIRON});
    CHECK(h.feed({IAC, SB, OPT_NEW_ENVIRON, ENV_SEND, IAC, SE}) ==
          Bytes{IAC, SB, OPT_NEW_ENVIRON, ENV_IS, ENV_USERVAR, 'X', ENV_VALUE, ENV_ESC, 0x00, ENV_ESC, 0x01, 0x41,
                ENV_ESC, 0x02, ENV_ESC, 0x03, IAC, IAC, IAC, SE});
}

TEST_CASE("tn5250 records: IAC EOR splits, IAC IAC undoubles, split reads join") {
    Harness h(TelnetSettings{});
    h.feed({0x00, 0x0B, 0x12, 0xA0, 0x00, 0x00, 0x04, 0x00, 0x00, 0x03, IAC});
    CHECK(h.records.empty());
    h.feed({IAC, IAC, NOP, IAC, EOR, 0x01});
    REQUIRE(h.records.size() == 1);
    CHECK(h.records[0] == Bytes{0x00, 0x0B, 0x12, 0xA0, 0x00, 0x00, 0x04, 0x00, 0x00, 0x03, 0xFF});
    // A negotiation command in the middle of a record does not end it.
    h.feed({0x02, IAC, DO, OPT_BINARY, 0x03, IAC, EOR});
    REQUIRE(h.records.size() == 2);
    CHECK(h.records[1] == Bytes{0x01, 0x02, 0x03});
    CHECK(TelnetCodec::frame({0x01, 0xFF, 0x02}) == Bytes{0x01, 0xFF, 0xFF, 0x02, IAC, EOR});
}

TEST_CASE("tn5250 records: header encode/decode, RFC 1205 message light example") {
    // RFC 1205 section 3: 000A 12A0 0000 0400 000B FFEF.
    Record r;
    std::string err;
    REQUIRE(decode_record({0x00, 0x0A, 0x12, 0xA0, 0x00, 0x00, 0x04, 0x00, 0x00, 0x0B}, r, err));
    CHECK(r.opcode == kOpMessageOn);
    CHECK(r.flags == 0);
    CHECK(r.data.empty());
    // RFC 1205 4.3: System Request record 000A12A0 00000404 0000.
    Record srq;
    srq.flags = kFlagSrq;
    CHECK(encode_record(srq) == Bytes{0x00, 0x0A, 0x12, 0xA0, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00});
    Record wsf;
    wsf.opcode = kOpPutGet;
    wsf.data = {0x04, 0xF3, 0x00, 0x05, 0xD9, 0x70, 0x00};
    const Bytes enc = encode_record(wsf);
    CHECK(enc == Bytes{0x00, 0x11, 0x12, 0xA0, 0x00, 0x00, 0x04, 0x00, 0x00, 0x03, 0x04, 0xF3, 0x00, 0x05, 0xD9, 0x70, 0x00});
    Record back;
    REQUIRE(decode_record(enc, back, err));
    CHECK(back.opcode == kOpPutGet);
    CHECK(back.data == wsf.data);
    CHECK_FALSE(decode_record({0x00, 0x0A, 0x12, 0xA1, 0, 0, 4, 0, 0, 0}, back, err));
    CHECK_FALSE(decode_record({0x00, 0x05, 0x12}, back, err));
}

TEST_CASE("tn5250 terminal type: derived from model and display, overridable per station") {
    using tn34xx::DisplaySubtype;
    CHECK(derived_terminal_type("3477", DisplaySubtype::Color) == "IBM-3477-FC");
    CHECK(derived_terminal_type("3477", DisplaySubtype::Green) == "IBM-3477-FG");
    CHECK(derived_terminal_type("3477", DisplaySubtype::Amber) == "IBM-3477-FG");
    CHECK(derived_terminal_type("3477", DisplaySubtype::PaperWhite) == "IBM-3477-FG");
    CHECK(derived_terminal_type("3486", DisplaySubtype::Green) == "IBM-5251-11");

    Tn5250Config cfg;
    CHECK(cfg.terminal_type.empty());          // default: derive
    StationConfig st;
    CHECK(station_terminal_type(cfg, st, "3477", DisplaySubtype::Amber) == "IBM-3477-FG");
    cfg.terminal_type = "IBM-3477-FC";         // --tn5250-type
    CHECK(station_terminal_type(cfg, st, "3477", DisplaySubtype::Amber) == "IBM-3477-FC");
    st.profile = "IBM-3179-2";                 // per-station profile wins
    CHECK(station_terminal_type(cfg, st, "3477", DisplaySubtype::Amber) == "IBM-3179-2");

    // The engine's capabilities follow the type: FG is 27x132 monochrome.
    const EngineOptions fg = EngineOptions::for_terminal_type(derived_terminal_type("3477", DisplaySubtype::Green));
    CHECK(fg.wide);
    CHECK_FALSE(fg.color);
    CHECK(fg.model == "0FG");

    // An empty type in the session's own settings means a colour 3477.
    Tn5250Config bare;
    bare.host = "h";
    CHECK(telnet_settings_for(bare).terminal_type == "IBM-3477-FC");
}
