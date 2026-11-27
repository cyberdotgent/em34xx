// SPDX-License-Identifier: GPL-3.0-or-later
// The TN3270 client end to end over a scripted transport: negotiation
// (RFC 1576: TERMINAL-TYPE, BINARY, EOR), a host write, a Query, and an
// attention key going back.
#include "tn3270/client.hpp"
#include "tn5250/telnet.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

using namespace em34xx;
using namespace em34xx::tn5250::telnet;

namespace {

// A host that sends a fixed byte sequence and records what it receives.
struct Wire {
    std::mutex mu;
    std::deque<uint8_t> to_client;
    std::vector<uint8_t> from_client;
};

class WireTransport : public tn5250::Transport {
public:
    explicit WireTransport(Wire& w) : w_(w) {}
    bool connect(const std::string&, uint16_t, std::string&) override { return true; }
    int recv(uint8_t* buf, size_t len, int timeout_ms) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms > 2 ? 2 : timeout_ms));
        std::lock_guard<std::mutex> g(w_.mu);
        size_t n = 0;
        while (n < len && !w_.to_client.empty()) {
            buf[n++] = w_.to_client.front();
            w_.to_client.pop_front();
        }
        return int(n);
    }
    bool send(const uint8_t* buf, size_t len) override {
        std::lock_guard<std::mutex> g(w_.mu);
        w_.from_client.insert(w_.from_client.end(), buf, buf + len);
        return true;
    }
    void close() override {}
    std::string describe() const override { return "wire"; }

private:
    Wire& w_;
};

void host_send(Wire& w, std::initializer_list<uint8_t> bytes) {
    std::lock_guard<std::mutex> g(w.mu);
    w.to_client.insert(w.to_client.end(), bytes.begin(), bytes.end());
}

bool wait_for(const std::function<bool()>& pred) {
    for (int i = 0; i < 500; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool contains(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

} // namespace

TEST_CASE("TN3270 client negotiates, applies a write, answers a query and sends an AID") {
    Wire wire;
    Tn5250Config cfg;
    cfg.host = "host.example";
    tn3270::Client client(cfg, [&](std::string&) { return std::make_unique<WireTransport>(wire); });
    std::string err;
    REQUIRE(client.start(err));
    host_send(wire, {IAC, DO, OPT_TTYPE, IAC, SB, OPT_TTYPE, TT_SEND, IAC, SE, IAC, DO, OPT_BINARY, IAC, WILL,
                     OPT_BINARY, IAC, DO, OPT_EOR, IAC, WILL, OPT_EOR});
    REQUIRE(wait_for([&] { return client.state() == tn3270::ConnState::Ready; }));
    {
        std::lock_guard<std::mutex> g(wire.mu);
        const std::string tt = "IBM-3279-2-E";
        std::vector<uint8_t> is{IAC, SB, OPT_TTYPE, TT_IS};
        is.insert(is.end(), tt.begin(), tt.end());
        CHECK(contains(wire.from_client, is));
    }
    // EW, WCC restore, SF(unprotected), then WSF Read Partition Query.
    host_send(wire, {0xF5, 0x02, 0x1D, 0x40, IAC, EOR, 0xF3, 0x00, 0x05, 0x01, IAC, IAC, 0x02, IAC, EOR});
    REQUIRE(wait_for([&] {
        client.pump();
        return client.engine().cell(0).fa;
    }));
    REQUIRE(wait_for([&] {
        client.pump();
        std::lock_guard<std::mutex> g(wire.mu);
        return contains(wire.from_client, {0x88});
    }));
    client.key(tn3270::Key::Home);
    CHECK(client.type(0xC1));
    client.aid(uint8_t(tn3270::Aid::Enter));
    REQUIRE(wait_for([&] {
        std::lock_guard<std::mutex> g(wire.mu);
        return contains(wire.from_client, {0x7D});
    }));
    {
        std::lock_guard<std::mutex> g(wire.mu);
        CHECK(contains(wire.from_client, {0xC1, IAC, EOR}));
    }
    // ATTN and SYSREQ are Telnet commands [RFC 2355 sections 11, 10.5.2].
    client.attn();
    client.sysreq();
    REQUIRE(wait_for([&] {
        std::lock_guard<std::mutex> g(wire.mu);
        return contains(wire.from_client, {IAC, IP}) && contains(wire.from_client, {IAC, AO});
    }));
    client.stop();
}
