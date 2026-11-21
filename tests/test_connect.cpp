// SPDX-License-Identifier: GPL-3.0-or-later
// The monitor's `connect` command line (config/connect_args.hpp).
#include "config/connect_args.hpp"

#include <doctest/doctest.h>

using namespace tn34xx;

namespace {
bool parse(std::vector<std::string> args, AppConfig& out, std::string& err, const AppConfig& base = AppConfig()) {
    args.insert(args.begin(), "connect");
    return parse_connect_args(args, base, out, err);
}
} // namespace

TEST_CASE("connect: host, port and TLS default port") {
    AppConfig c;
    std::string err;
    REQUIRE(parse({"pub400.com"}, c, err));
    CHECK(c.tn5250.host == "pub400.com");
    CHECK(c.tn5250.port == 23);
    CHECK_FALSE(c.tn5250.tls);
    CHECK(c.twinax.host == TwinaxHostKind::Tn5250);
    REQUIRE(c.twinax.effective_stations().size() == 1);
    CHECK(c.twinax.effective_stations()[0].address == 0);

    REQUIRE(parse({"192.0.2.10:2323"}, c, err));
    CHECK(c.tn5250.host == "192.0.2.10");
    CHECK(c.tn5250.port == 2323);

    REQUIRE(parse({"ibmi.example", "tls"}, c, err));
    CHECK(c.tn5250.tls);
    CHECK(c.tn5250.port == 992);
    REQUIRE(parse({"ibmi.example:8992", "TLS"}, c, err));
    CHECK(c.tn5250.port == 8992);
    REQUIRE(parse({"ibmi.example", "tls", "port", "2992"}, c, err));
    CHECK(c.tn5250.port == 2992);

    REQUIRE(parse({"[::1]:2323"}, c, err));
    CHECK(c.tn5250.host == "::1");
    CHECK(c.tn5250.port == 2323);
}

TEST_CASE("connect: station, device, type and passed-through options") {
    AppConfig c;
    std::string err;
    REQUIRE(parse({"h", "station", "1", "device", "QPADEV77", "type", "IBM-3179-2", "codepage", "273"}, c, err));
    const auto st = c.twinax.effective_stations();
    REQUIRE(st.size() == 1);
    CHECK(st[0].address == 1);
    CHECK(st[0].type == StationType::Display);
    CHECK(c.tn5250.device_name == "QPADEV77");
    CHECK(c.tn5250.terminal_type == "IBM-3179-2");
    CHECK(c.tn5250.codepage == "273");

    // Generic NAME VALUE / NAME=VALUE go to --tn5250-NAME.
    REQUIRE(parse({"h", "tls", "ca", "/etc/ca.pem", "env=USER=QUSER"}, c, err));
    CHECK(c.tn5250.ca_file == "/etc/ca.pem");
    CHECK(c.tn5250.env["USER"] == "QUSER");
}

TEST_CASE("connect: defaults come from the start-up configuration") {
    AppConfig base;
    base.tn5250.codepage = "500";
    base.tn5250.device_name = "DEFAULT";
    base.twinax.stations = {StationConfig{0, StationType::Display, "", ""},
                            StationConfig{1, StationType::Printer, "PRT", ""}};
    AppConfig c;
    std::string err;
    REQUIRE(parse({"h"}, c, err, base));
    CHECK(c.tn5250.codepage == "500");
    CHECK(c.tn5250.device_name == "DEFAULT");
    CHECK(c.twinax.effective_stations().size() == 2);
    REQUIRE(parse({"h", "device", "OTHER"}, c, err, base));
    CHECK(c.tn5250.device_name == "OTHER");
    // station N picks one display station from the list.
    REQUIRE(parse({"h", "station", "0"}, c, err, base));
    CHECK(c.twinax.effective_stations().size() == 1);
}

TEST_CASE("connect: errors") {
    AppConfig c;
    std::string err;
    CHECK_FALSE(parse({}, c, err));
    CHECK(err.find("usage") != std::string::npos);
    CHECK_FALSE(parse({"h", "station"}, c, err));
    CHECK_FALSE(parse({"h", "station", "7"}, c, err));
    CHECK_FALSE(parse({"h", "bogus-option"}, c, err));
    CHECK(err.find("unknown option") != std::string::npos);
    CHECK_FALSE(parse({"h:notaport"}, c, err));
    CHECK_FALSE(parse({"[::1"}, c, err));
}
