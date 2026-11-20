#include "host/twinax_link.hpp"

#include <doctest/doctest.h>

using namespace tn34xx;

TEST_CASE("twinax frame encoding round-trips") {
    for (unsigned addr = 0; addr < 8; ++addr) {
        for (unsigned d = 0; d < 256; ++d) {
            TwinaxFrame f{uint8_t(d), uint8_t(addr)};
            const uint16_t w = f.encode();
            CHECK((w & 0x8000) != 0);          // sync bit
            CHECK((w & 0x0007) == 0);          // fill
            unsigned ones = 0;
            for (uint16_t x = w; x; x &= uint16_t(x - 1)) ++ones;
            CHECK(ones % 2 == 0);              // even parity
            TwinaxFrame g;
            bool parity_ok = false;
            REQUIRE(TwinaxFrame::decode(w, g, parity_ok));
            CHECK(parity_ok);
            CHECK(g.data == f.data);
            CHECK(g.address == f.address);
        }
    }
}

TEST_CASE("data byte is sent least significant bit first") {
    TwinaxFrame f{0x01, 0};
    CHECK((f.encode() & 0x4000) != 0);   // data bit 0 follows the sync bit
    TwinaxFrame g{0x80, 0};
    CHECK((g.encode() & 0x0080) != 0);   // data bit 7 is the last data cell
}

TEST_CASE("station address is sent least significant bit first") {
    // IBM 5250 Product Attachment Information: the address follows the data
    // in the same LSB-first order, so address bit 0 is frame bit 6.
    CHECK((TwinaxFrame{0x00, 1}.encode() & 0x0070) == 0x0040);
    CHECK((TwinaxFrame{0x00, 4}.encode() & 0x0070) == 0x0010);
    CHECK((TwinaxFrame{0x00, 6}.encode() & 0x0070) == 0x0030);
}

TEST_CASE("parity errors and missing sync are reported") {
    TwinaxFrame f{0x5A, 3};
    TwinaxFrame g;
    bool parity_ok = true;
    REQUIRE(TwinaxFrame::decode(uint16_t(f.encode() ^ 0x0008), g, parity_ok));
    CHECK_FALSE(parity_ok);
    CHECK_FALSE(TwinaxFrame::decode(0x0000, g, parity_ok));
}

TEST_CASE("script host queues and records") {
    ScriptTwinaxHost h;
    TwinaxMessage out;
    CHECK_FALSE(h.to_station(out));
    h.queue({{0x01, 0}, {0x00, 7}});
    REQUIRE(h.to_station(out));
    CHECK(out.size() == 2);
    CHECK(out[1].end_of_message());
    h.from_station({{0x42, 0}});
    auto rx = h.take_received();
    REQUIRE(rx.size() == 1);
    CHECK(rx[0][0].data == 0x42);
    CHECK(format_twinax_message(rx[0]) == "42/0");
}

TEST_CASE("tn5250 host requires configuration") {
    TwinaxConfig tw;
    tw.host = TwinaxHostKind::Tn5250;
    Tn5250Config tn;
    std::string err;
    CHECK(make_twinax_host(tw, tn, err) == nullptr);
    tn.host = "ibmi.example";
    auto h = make_twinax_host(tw, tn, err);
    REQUIRE(h != nullptr);
    CHECK(std::string(h->kind()) == "tn5250");
    tw.station_address = 7;
    CHECK(make_twinax_host(tw, tn, err) == nullptr);
}

TEST_CASE("message station and host events") {
    CHECK(twinax_message_station({{0x10, 3}}) == 3);
    CHECK(twinax_message_station({{0x13, 1}, {0x00, 7}}) == 1);
    CHECK(twinax_message_station({}) == 7);
    ScriptTwinaxHost h;
    h.advance(1000);
    h.no_response();
    CHECK(h.describe().find("1 without response") != std::string::npos);
}
