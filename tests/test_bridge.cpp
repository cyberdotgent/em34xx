// End to end through the TN5250 bridge: a scripted in-process TN5250 server
// -> tn5250::Client (its own I/O thread) -> engine -> station controller ->
// link scheduler -> FakeStation, and keystrokes back the same way.
#include "fake_station.hpp"
#include "tn5250_test_util.hpp"

#include "host/wsc/tn5250_bridge.hpp"
#include "tn5250/record.hpp"
#include "tn5250/telnet.hpp"
#include "tn5250/transport.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace tn34xx;
using namespace tn34xx::wsc;
using namespace tn34xx::tn5250::telnet;
using tn34xx::test::FakeStation;
using tn34xx::test::FakeStationOptions;
using tn5250_test::Bytes;

namespace {

// The server's side of one connection. The client's I/O thread calls the
// Transport methods; the test thread reads what the client sent.
struct ServerScript {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Bytes> to_client;      // one chunk per recv()
    Bytes from_client;
    bool connected = false;
    bool closed = false;              // the server hangs up
    std::string host;
    uint16_t port = 0;

    void push(Bytes b) {
        std::lock_guard<std::mutex> g(mu);
        to_client.push_back(std::move(b));
        cv.notify_all();
    }
    Bytes sent() {
        std::lock_guard<std::mutex> g(mu);
        return from_client;
    }
};

class ScriptTransport : public tn34xx::tn5250::Transport {
public:
    explicit ScriptTransport(std::shared_ptr<ServerScript> s) : s_(std::move(s)) {}
    bool connect(const std::string& host, uint16_t port, std::string&) override {
        std::lock_guard<std::mutex> g(s_->mu);
        s_->connected = true;
        s_->host = host;
        s_->port = port;
        return true;
    }
    int recv(uint8_t* buf, size_t len, int timeout_ms) override {
        std::unique_lock<std::mutex> g(s_->mu);
        s_->cv.wait_for(g, std::chrono::milliseconds(timeout_ms),
                        [&] { return !s_->to_client.empty() || s_->closed; });
        if (s_->to_client.empty()) return s_->closed ? -1 : 0;
        Bytes& front = s_->to_client.front();
        const size_t n = std::min(len, front.size());
        std::copy(front.begin(), front.begin() + std::ptrdiff_t(n), buf);
        front.erase(front.begin(), front.begin() + std::ptrdiff_t(n));
        if (front.empty()) s_->to_client.pop_front();
        return int(n);
    }
    bool send(const uint8_t* buf, size_t len) override {
        std::lock_guard<std::mutex> g(s_->mu);
        s_->from_client.insert(s_->from_client.end(), buf, buf + len);
        return true;
    }
    void close() override {}
    std::string describe() const override { return "script"; }

private:
    std::shared_ptr<ServerScript> s_;
};

// The negotiation an IBM i server runs (RFC 1205 order), in one chunk.
Bytes negotiation() {
    return {IAC, DO, OPT_TTYPE, IAC, SB, OPT_TTYPE, TT_SEND, IAC, SE, IAC, DO, OPT_EOR, IAC, WILL, OPT_EOR,
            IAC, DO, OPT_BINARY, IAC, WILL, OPT_BINARY};
}

Bytes framed_record(const Bytes& data, uint8_t opcode = tn34xx::tn5250::kOpPutGet) {
    tn34xx::tn5250::Record r;
    r.opcode = opcode;
    r.data = data;
    return tn34xx::tn5250::TelnetCodec::frame(encode_record(r));
}

// Records in the client's byte stream (telnet commands skipped).
std::vector<tn34xx::tn5250::Record> client_records(const Bytes& in) {
    std::vector<tn34xx::tn5250::Record> out;
    Bytes cur;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != IAC) { cur.push_back(in[i]); continue; }
        if (i + 1 >= in.size()) break;
        const uint8_t c = in[i + 1];
        if (c == IAC) { cur.push_back(IAC); ++i; }
        else if (c == EOR) {
            tn34xx::tn5250::Record r;
            std::string why;
            if (tn34xx::tn5250::decode_record(cur, r, why)) out.push_back(r);
            cur.clear();
            ++i;
        } else if (c == SB) {
            while (i + 1 < in.size() && !(in[i] == IAC && in[i + 1] == SE)) ++i;
            ++i;
        } else if (c == WILL || c == WONT || c == DO || c == DONT) i += 2;
        else ++i;
    }
    return out;
}

bool contains(const Bytes& hay, const std::string& needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

// The cable, as in test_wsc.cpp, around any TwinaxHost. Waits also pass
// wall time, because the client's I/O thread runs on its own.
struct Bench {
    TwinaxHost& host;
    std::map<uint8_t, FakeStation*> cable;
    uint64_t t = 0;

    explicit Bench(TwinaxHost& h) : host(h) {}
    void plug(FakeStation& s) { cable[s.address()] = &s; }
    void step() {
        host.advance(t);
        TwinaxMessage m;
        if (!host.to_station(m)) { t += 100'000; return; }
        t += 16'000 * m.size();
        TwinaxMessage r;
        auto it = cable.find(twinax_message_station(m));
        if (it != cable.end() && it->second->receive(m, r)) {
            t += 45'000 + 16'000 * r.size();
            host.advance(t);
            host.from_station(r);
        } else {
            t += 60'000;
            host.advance(t);
            host.no_response();
        }
    }
    bool run_until(const std::function<bool()>& done, int wall_ms = 5000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wall_ms);
        while (std::chrono::steady_clock::now() < end) {
            for (int k = 0; k < 200; ++k) {
                if (done()) return true;
                step();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    }
    void run_for_emulated(uint64_t ns) {
        const uint64_t end = t + ns;
        while (t < end) step();
    }
};

std::string row_text(const FakeStation& f, unsigned row, unsigned col, size_t n) {
    const auto* cp = tn34xx::tn5250::CodePage::find("37");
    std::string s;
    for (size_t k = 0; k < n; ++k) {
        const uint8_t b = f.cell(row, unsigned(col + k));
        s += b >= 0x40 ? char(cp->to_unicode(b)) : ' ';
    }
    return s;
}

} // namespace

TEST_CASE("tn5250 bridge: sign-on screen to the station, keystrokes back to the host") {
    TwinaxConfig tw;
    tw.host = TwinaxHostKind::Tn5250;
    tw.stations = {StationConfig{0, StationType::Display, "TN34XXA", ""},
                   StationConfig{1, StationType::Printer, "TN34XXP", ""}};
    Tn5250Config tn;
    tn.host = "ibmi.example";
    tn.port = 2323;

    auto server = std::make_shared<ServerScript>();
    server->push(negotiation());
    server->push(framed_record(tn5250_test::signon_stream()));
    int connects = 0;
    std::vector<std::string> station_devnames;
    BridgeOptions opt;
    opt.model = "3477";
    opt.display = DisplaySubtype::Green;
    opt.reconnect_min_ms = 50;
    opt.transport = [&](const StationConfig& st, std::string&) -> std::unique_ptr<tn34xx::tn5250::Transport> {
        ++connects;
        station_devnames.push_back(st.device_name);
        return std::make_unique<ScriptTransport>(server);
    };
    std::string err;
    auto host = make_tn5250_bridge(tw, tn, opt, err);
    REQUIRE(host);
    auto& bridge = static_cast<Tn5250Bridge&>(*host);
    CHECK(std::string(bridge.kind()) == "tn5250");
    REQUIRE(bridge.display(0));
    REQUIRE(bridge.printer(1));
    CHECK(bridge.display(1) == nullptr);
    CHECK(bridge.display(0)->config().terminal_type == "IBM-3477-FG");
    CHECK(bridge.display(0)->config().device_name == "TN34XXA");

    Bench b(bridge);
    // Nothing on the cable yet: no session is started.
    b.run_for_emulated(2'000'000'000);
    CHECK(connects == 0);

    FakeStation f(0);
    FakeStationOptions popt;
    popt.type = StationType::Printer;
    FakeStation p(1, popt);
    b.plug(f);
    b.plug(p);

    // The station comes online, the session starts, and the sign-on panel
    // reaches the station's buffer.
    REQUIRE(b.run_until([&] { return row_text(f, 0, 35, 7) == "Sign On"; }));
    CHECK(connects == 1);
    REQUIRE(station_devnames.size() == 1);
    CHECK(station_devnames[0] == "TN34XXA");
    CHECK(server->host == "ibmi.example");
    CHECK(server->port == 2323);
    CHECK(row_text(f, 5, 16, 4) == "User");
    CHECK(f.cell(5, 51) == 0x24);   // the user field's attribute
    // Cursor at the insert-cursor position once the paint has finished.
    CHECK(b.run_until([&] { return f.cursor() == 5u * 80 + 52; }));
    const Bytes neg = server->sent();
    CHECK(contains(neg, "IBM-3477-FG"));

    // The printer is identified and polled; its placeholder takes no data.
    REQUIRE(b.run_until([&] { return bridge.printer(1)->ready(); }));
    CHECK(p.printed().empty());
    CHECK(bridge.describe().find("printer: session not implemented") != std::string::npos);

    // Type 'a' (monocase field: A) and Enter on the station's keyboard.
    REQUIRE(b.run_until([&] { return (f.indicators() & 0x02) == 0; }));   // keyboard unlocked
    f.press(0x1C);
    f.press(0x58);
    std::vector<tn34xx::tn5250::Record> recs;
    REQUIRE(b.run_until([&] {
        recs = client_records(server->sent());
        return !recs.empty();
    }));
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].opcode == tn34xx::tn5250::kOpPutGet);
    // Cursor 06/36, AID Enter, then SBA 06/35 and the field's data.
    CHECK(recs[0].data == Bytes{0x06, 0x36, 0xF1, 0x11, 0x06, 0x35, 0xC1});
    // The typed character was painted on the station as well.
    CHECK(f.cell(5, 52) == 0xC1);

    // Device lost and back: the same session, the screen repainted.
    f.set_powered(false);
    REQUIRE(b.run_until([&] { return !bridge.display(0)->online(); }));
    f.set_powered(true);
    REQUIRE(b.run_until([&] { return bridge.display(0)->online() && row_text(f, 0, 35, 7) == "Sign On"; }));
    CHECK(bridge.display(0)->sessions_started() == 1);
    CHECK(connects == 1);

    // The host hangs up: the bridge reconnects with a fresh session.
    {
        std::lock_guard<std::mutex> g(server->mu);
        server->closed = true;
        server->cv.notify_all();
    }
    REQUIRE(b.run_until([&] {
        return bridge.display(0)->client()->state() == tn34xx::tn5250::ConnState::Closed;
    }));
    {
        std::lock_guard<std::mutex> g(server->mu);
        server->closed = false;
        server->to_client.push_back(negotiation());
    }
    REQUIRE(b.run_until([&] { return connects == 2; }, 8000));
    CHECK(bridge.display(0)->sessions_started() == 2);
}

TEST_CASE("tn5250 bridge: per-station session settings") {
    Tn5250Config tn;
    tn.host = "h";
    tn.device_name = "SHARED";
    tn.codepage = "273";
    StationConfig a{0, StationType::Display, "", ""};
    StationConfig b{1, StationType::Display, "SECOND", "IBM-3179-2"};
    StationConfig c{2, StationType::Display, "", ""};
    const Tn5250Config ca = station_session_config(tn, a, true, "3477", DisplaySubtype::Color);
    CHECK(ca.device_name == "SHARED");
    CHECK(ca.terminal_type == "IBM-3477-FC");
    CHECK(ca.codepage == "273");
    const Tn5250Config cb = station_session_config(tn, b, false, "3477", DisplaySubtype::Color);
    CHECK(cb.device_name == "SECOND");
    CHECK(cb.terminal_type == "IBM-3179-2");
    const Tn5250Config cc = station_session_config(tn, c, false, "3477", DisplaySubtype::Green);
    CHECK(cc.device_name.empty());
    CHECK(cc.terminal_type == "IBM-3477-FG");
}

TEST_CASE("tn5250 bridge: make_twinax_host uses the bridge factory") {
    TwinaxConfig tw;
    tw.host = TwinaxHostKind::Tn5250;
    Tn5250Config tn;
    tn.host = "ibmi.example";
    std::string err;
    bool called = false;
    auto h = make_twinax_host(tw, tn, err, [&](const TwinaxConfig& t, const Tn5250Config& c, std::string& e) {
        called = true;
        return make_tn5250_bridge(t, c, BridgeOptions(), e);
    });
    REQUIRE(h);
    CHECK(called);
    CHECK(dynamic_cast<Tn5250Bridge*>(h.get()) != nullptr);
    CHECK(h->describe().find("station 0 display: session idle") != std::string::npos);
}
