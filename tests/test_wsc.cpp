// Virtual workstation controller (src/host/wsc) against fake stations.
#include "fake_station.hpp"

#include "host/wsc/device_commands.hpp"
#include "host/wsc/keyboard_map.hpp"
#include "host/wsc/link_scheduler.hpp"
#include "host/wsc/screen_writer.hpp"
#include "host/wsc/tn5250_adapter.hpp"

#include <doctest/doctest.h>

#include <cstring>
#include <functional>
#include <map>
#include <memory>

using namespace tn34xx;
using namespace tn34xx::wsc;
using tn34xx::test::FakeStation;
using tn34xx::test::FakeStationOptions;

namespace {

constexpr uint64_t kMs = 1'000'000;

uint8_t ebcdic(char ch) {
    if (ch >= 'A' && ch <= 'I') return uint8_t(0xC1 + (ch - 'A'));
    if (ch >= 'J' && ch <= 'R') return uint8_t(0xD1 + (ch - 'J'));
    if (ch >= 'S' && ch <= 'Z') return uint8_t(0xE2 + (ch - 'S'));
    if (ch >= '0' && ch <= '9') return uint8_t(0xF0 + (ch - '0'));
    return 0x40;
}

// A display session double: a shadow screen the test edits, and a simple
// echo of data keys, standing in for the 5250 engine (src/tn5250/).
struct TestDisplay : DisplaySessionPort {
    ScreenState screen;
    uint64_t generation = 1;
    bool alarm = false;
    bool echo = true;
    int ready_count = 0;
    int lost_count = 0;
    StationIdentity id;
    std::vector<OperatorAction> actions;

    TestDisplay() { resize(24, 80); }
    void resize(unsigned r, unsigned c) {
        screen.rows = r;
        screen.cols = c;
        screen.cells.assign(size_t(r) * c, 0);
        ++generation;
    }
    void put(unsigned row, unsigned col, const char* text) {
        for (size_t k = 0; text[k]; ++k) screen.cells[row * screen.cols + col + k] = ebcdic(text[k]);
        ++generation;
    }
    void put_byte(unsigned addr, uint8_t b) { screen.cells[addr] = b; ++generation; }
    void set_cursor(unsigned a) { screen.cursor = a; ++generation; }

    void device_ready(const StationIdentity& i) override { id = i; ++ready_count; }
    void device_lost() override { ++lost_count; }
    uint64_t screen_generation() const override { return generation; }
    void screen_snapshot(ScreenState& out) const override { out = screen; }
    bool take_alarm() override { const bool a = alarm; alarm = false; return a; }
    void operator_action(const OperatorAction& a) override {
        actions.push_back(a);
        if (echo && a.kind == OperatorAction::Kind::Character) {
            screen.cells[screen.cursor] = a.ebcdic;
            ++screen.cursor;
            ++generation;
        }
    }
};

struct TestPrinter : PrinterSessionPort {
    int ready_count = 0;
    int lost_count = 0;
    StationIdentity id;
    std::vector<std::vector<uint8_t>> jobs;
    std::vector<uint8_t> statuses;
    void device_ready(const StationIdentity& i) override { id = i; ++ready_count; }
    void device_lost() override { ++lost_count; }
    bool next_print_data(std::vector<uint8_t>& scs) override {
        if (jobs.empty()) return false;
        scs = jobs.front();
        jobs.erase(jobs.begin());
        return true;
    }
    void printer_status(uint8_t b) override { statuses.push_back(b); }
};

// The cable and the link device's role: deliver messages in emulated time,
// with frame and turnaround delays, and report responses or silence.
struct Bench {
    std::unique_ptr<LinkScheduler> host;
    std::map<uint8_t, FakeStation*> cable;
    std::vector<TwinaxMessage> sent;   // log of controller messages
    uint64_t t = 0;

    explicit Bench(const std::vector<StationConfig>& cfg) : host(std::make_unique<LinkScheduler>(cfg)) {}
    void plug(FakeStation& s) { cable[s.address()] = &s; }

    void step() {
        host->advance(t);
        TwinaxMessage m;
        if (!host->to_station(m)) {
            t += 100'000;   // idle line
            return;
        }
        sent.push_back(m);
        t += 16'000 * m.size();
        TwinaxMessage r;
        auto it = cable.find(twinax_message_station(m));
        if (it != cable.end() && it->second->receive(m, r)) {
            t += 45'000 + 16'000 * r.size();
            host->advance(t);
            host->from_station(r);
        } else {
            t += 60'000;
            host->advance(t);
            host->no_response();
        }
    }
    void run_for(uint64_t ns) {
        const uint64_t end = t + ns;
        while (t < end) step();
    }
    bool run_until(const std::function<bool()>& done, uint64_t limit = 3000 * kMs) {
        const uint64_t end = t + limit;
        while (t < end) {
            if (done()) return true;
            step();
        }
        return done();
    }
};

StationConfig display_at(unsigned a) { return StationConfig{a, StationType::Display, "", ""}; }
StationConfig printer_at(unsigned a) { return StationConfig{a, StationType::Printer, "", ""}; }

bool screen_matches(const FakeStation& f, const ScreenState& s) {
    if (f.cols() != s.cols || f.rows() != s.rows) return false;
    for (size_t i = 0; i < s.cells.size(); ++i)
        if (f.ps()[i] != s.cells[i]) return false;
    return f.cursor() == s.cursor && f.indicators() == s.indicators;
}

} // namespace

TEST_CASE("queue loads and message framing") {
    const uint8_t text[] = {0xC8, 0xC5};
    QueueLoad q;
    q.load_address(0x123).write_data_load_cursor(text, 2).end_of_queue();
    const std::vector<uint8_t> want = {0x15, 0x01, 0x23, 0x11, 0x02, 0xC8, 0xC5, 0x62};
    CHECK(q.bytes() == want);
    const TwinaxMessage m = make_message(q.bytes(), 3);
    REQUIRE(m.size() == want.size());
    CHECK(m.front().address == 3);
    CHECK(m[6].address == 3);
    CHECK(m.back().address == 7);
    CHECK(make_message({cmd::ReadDeviceId}, 2).front().address == 2);   // single frame: station address
    CHECK(make_poll(1, false).front().data == 0x30);
    CHECK(make_poll(1, true).front().data == 0x70);
    // WCD with a second byte sets "byte 2 follows".
    CHECK(QueueLoad().write_control(0x00, 0x0A).bytes() == std::vector<uint8_t>{0x05, 0x40, 0x0A});
}

TEST_CASE("flush planner: short runs as WDLC, long runs as Write Data") {
    ScreenState s;
    s.cells.assign(1920, 0);
    DeviceMirror m;
    m.set_format(24, 80);
    std::fill(m.cells.begin(), m.cells.end(), int16_t(0));
    m.cursor = 0;
    m.indicators = s.indicators;
    m.wcd_byte1 = 0;
    m.wcd_byte2 = 0;
    s.cells[81] = 0xC1;   // a short change at row 1
    for (unsigned i = 400; i < 470; ++i) s.cells[i] = 0xC2;   // a long run
    s.cursor = 0;
    const auto steps = plan_flush(s, m, false, FlushOptions());
    REQUIRE(steps.size() == 3);   // WDLC, Write Data, cursor (WDLC moved it)
    CHECK(steps[0].load == std::vector<uint8_t>{0x15, 0x00, 0x51, 0x11, 0x01, 0xC1, 0x62});
    CHECK(steps[1].load == std::vector<uint8_t>{0x15, 0x01, 0x90, 0x1E});
    CHECK(steps[1].payload.size() == 70);
    CHECK(steps[2].load == std::vector<uint8_t>{0x17, 0x00, 0x00, 0x62});
    for (const Step& st : steps) CHECK(st.load.size() <= kQueueBytes);
}

TEST_CASE("keyboard map: Enhanced keyboard, shift and caps lock") {
    KeyboardMap km(0x82);
    OperatorAction a;
    REQUIRE(km.translate(0x1C, a));   // a
    CHECK(a.kind == OperatorAction::Kind::Character);
    CHECK(a.ebcdic == 0x81);
    CHECK_FALSE(km.translate(0x12, a));   // left shift make
    REQUIRE(km.translate(0x1C, a));
    CHECK(a.ebcdic == 0xC1);
    REQUIRE(km.translate(0x16, a));       // shift 1 = !
    CHECK(a.ebcdic == 0x5A);
    CHECK_FALSE(km.translate(0x92, a));   // left shift break
    REQUIRE(km.translate(0x07, a));       // F1
    CHECK(a.kind == OperatorAction::Kind::Aid);
    CHECK(a.aid == 0x31);
    REQUIRE(km.translate(0x58, a));       // Enter
    CHECK(a.aid == 0xF1);
    REQUIRE(km.translate(0x5A, a));
    CHECK(a.function == KeyFunction::FieldExit);
    CHECK_FALSE(km.translate(0x14, a));   // caps lock
    REQUIRE(km.translate(0x16, a));       // digits ignore caps lock
    CHECK(a.ebcdic == 0xF1);
    REQUIRE(km.translate(0x1C, a));
    CHECK(a.ebcdic == 0xC1);
    REQUIRE(km.translate(0xFF, a));
    CHECK(a.kind == OperatorAction::Kind::Overrun);
    KeyboardMap typewriter(0x02);
    REQUIRE(typewriter.translate(0x1C, a));
    CHECK(a.kind == OperatorAction::Kind::Unmapped);
    CHECK(a.scan == 0x1C);
}

TEST_CASE("discovery handshake identifies a 3477 display") {
    Bench b({display_at(0)});
    FakeStation f(0);
    TestDisplay d;
    b.plug(f);
    b.host->station(0)->attach_display(&d);
    REQUIRE(b.run_until([&] { return d.ready_count == 1; }));
    Station& st = *b.host->station(0);
    CHECK(st.state() == StationState::Online);
    CHECK(st.identity().device_id == 0xC2);
    CHECK(st.identity().model_id == 0x24);
    CHECK(st.identity().keyboard_id == 0x82);
    CHECK(st.identity().enhanced_keyboard());
    CHECK(st.identity().has_extended_id);
    CHECK(st.identity().is_3477());
    CHECK(st.identity().wide_capable());
    CHECK_FALSE(st.identity().colour());
    CHECK(f.set_mode_done());
    CHECK(f.exception() == 0);
    CHECK(f.queue_size() == 64);      // WCD byte 2 bit 02h on a 3477
    // The sequence starts with a poll, then Set Mode (no fill).
    REQUIRE(b.sent.size() > 3);
    CHECK(b.sent[0].front().data == 0x30);
    CHECK(b.sent[1].size() == 2);
    CHECK(b.sent[1][0].data == 0x13);
    CHECK(b.sent[1][1].data == 0x00);
    CHECK(b.sent[1][1].address == 7);
    // Model ID (8C) is read before any WCD.
    size_t model = 0, wcd = 0;
    for (size_t i = 0; i < b.sent.size(); ++i) {
        if (!model && b.sent[i][0].data == 0x8C) model = i;
        if (!wcd && b.sent[i][0].data == 0x05) wcd = i;
    }
    CHECK(model != 0);
    CHECK(model < wcd);
}

TEST_CASE("colour model and a non-3477 station") {
    FakeStationOptions o;
    o.model_id = 0x50;
    o.is_3477 = false;   // refuses Read Info: exception 08, which identification skips
    Bench b({display_at(2)});
    FakeStation f(2, o);
    TestDisplay d;
    b.plug(f);
    b.host->station(2)->attach_display(&d);
    REQUIRE(b.run_until([&] { return d.ready_count == 1; }));
    CHECK(d.id.colour());
    CHECK_FALSE(d.id.is_3477());
    CHECK(d.id.vpd.empty());
    CHECK(f.queue_size() == 16);
    CHECK(f.exception() == 0);
}

TEST_CASE("screen paint through the mirror diff") {
    Bench b({display_at(0)});
    FakeStation f(0);
    TestDisplay d;
    b.plug(f);
    b.host->station(0)->attach_display(&d);
    REQUIRE(b.run_until([&] { return d.ready_count == 1; }));

    d.put(0, 2, "SIGN ON");
    d.put_byte(5 * 80 + 9, 0x20);   // field attribute
    d.put(5, 10, "USER");
    for (unsigned i = 0; i < 100; ++i) d.put_byte(10 * 80 + i, ebcdic(char('A' + i % 26)));   // a long run
    d.screen.indicators = 0x00;   // keyboard unlocked
    d.set_cursor(6 * 80 + 10);
    REQUIRE(b.run_until([&] { return screen_matches(f, d.screen); }));
    CHECK(b.run_until([&] { return b.host->station(0)->mirror().matches(d.screen); }));

    // A one-character change costs one small load, not a repaint.
    b.run_for(100 * kMs);
    const unsigned loads = f.loads();
    d.put(0, 2, "Z");
    d.set_cursor(6 * 80 + 10);
    REQUIRE(b.run_until([&] { return screen_matches(f, d.screen); }));
    b.run_for(50 * kMs);
    CHECK(f.loads() - loads <= 2);
    CHECK(f.cell(0, 2) == ebcdic('Z'));

    // Alarm and 27x132.
    d.alarm = true;
    d.resize(27, 132);
    d.put(26, 120, "WIDE");
    d.set_cursor(0);
    REQUIRE(b.run_until([&] { return screen_matches(f, d.screen); }));
    CHECK(f.alarms() == 1);
    CHECK((f.wcd_byte2() & 0x08) != 0);
}

TEST_CASE("keystroke round trip") {
    Bench b({display_at(0)});
    FakeStation f(0);
    TestDisplay d;
    b.plug(f);
    b.host->station(0)->attach_display(&d);
    REQUIRE(b.run_until([&] { return d.ready_count == 1; }));
    d.set_cursor(3 * 80 + 5);
    b.run_for(50 * kMs);

    f.press(0x12);   // shift make
    f.press(0x1C);   // A
    f.press(0x92);   // shift break
    f.press(0x1B);   // s
    f.press(0x58);   // Enter
    REQUIRE(b.run_until([&] { return d.actions.size() == 3; }));
    CHECK(d.actions[0].ebcdic == 0xC1);
    CHECK(d.actions[1].ebcdic == 0xA2);
    CHECK(d.actions[2].kind == OperatorAction::Kind::Aid);
    CHECK(d.actions[2].aid == 0xF1);
    // The echo reached the device; each key was delivered once.
    REQUIRE(b.run_until([&] { return screen_matches(f, d.screen); }));
    CHECK(f.cell(3, 5) == 0xC1);
    CHECK(f.cell(3, 6) == 0xA2);
    CHECK(f.cursor() == 3 * 80 + 7);
    b.run_for(200 * kMs);
    CHECK(d.actions.size() == 3);
    CHECK(b.host->station(0)->counters().keys == 5);
}

TEST_CASE("two display stations keep independent state") {
    Bench b({display_at(0), display_at(1)});
    FakeStation f0(0), f1(1);
    TestDisplay d0, d1;
    b.plug(f0);
    b.plug(f1);
    b.host->station(0)->attach_display(&d0);
    b.host->station(1)->attach_display(&d1);
    REQUIRE(b.run_until([&] { return d0.ready_count == 1 && d1.ready_count == 1; }));

    d0.put(0, 0, "SESSION A");
    d1.put(1, 0, "SESSION B");
    d1.set_cursor(500);
    REQUIRE(b.run_until([&] { return screen_matches(f0, d0.screen) && screen_matches(f1, d1.screen); }));
    CHECK(f0.cell(1, 0) == 0);
    CHECK(f1.cell(0, 0) == 0);

    f1.press(0x2C);   // t, only on session B
    REQUIRE(b.run_until([&] { return d1.actions.size() == 1; }));
    b.run_for(100 * kMs);
    CHECK(d0.actions.empty());
    CHECK(f1.cell(6, 20) == 0xA3);

    // Both stations stay polled well inside the 3477's 200 ms window.
    const uint64_t p0 = b.host->station(0)->counters().polls;
    const uint64_t p1 = b.host->station(1)->counters().polls;
    b.run_for(200 * kMs);
    CHECK(b.host->station(0)->counters().polls - p0 >= 6);
    CHECK(b.host->station(1)->counters().polls - p1 >= 6);
}

TEST_CASE("display and printer sessions at the same time") {
    Bench b({display_at(0), printer_at(1)});
    FakeStationOptions po;
    po.type = StationType::Printer;
    po.printer_id = 0x2D;   // 5219, 3812 SCS emulation
    FakeStation fd(0), fp(1, po);
    TestDisplay d;
    TestPrinter p;
    b.plug(fd);
    b.plug(fp);
    b.host->station(0)->attach_display(&d);
    b.host->station(1)->attach_printer(&p);
    REQUIRE(b.run_until([&] { return d.ready_count == 1 && p.ready_count == 1; }));
    CHECK(p.id.type == StationType::Printer);
    CHECK(std::strcmp(printer_type_name(p.id.device_id), "5219") == 0);
    CHECK(d.id.type == StationType::Display);

    std::vector<uint8_t> job(600);
    for (size_t i = 0; i < job.size(); ++i) job[i] = uint8_t(0x40 + i % 64);
    p.jobs.push_back(job);
    d.put(0, 0, "PRINTING");
    REQUIRE(b.run_until([&] { return fp.printed() == job && screen_matches(fd, d.screen); }));
    fp.printer_status = 0x10;
    REQUIRE(b.run_until([&] { return !p.statuses.empty() && p.statuses.back() == 0x10; }));
}

TEST_CASE("configured type that the device contradicts") {
    Bench b({printer_at(0)});
    FakeStation f(0);   // a display
    TestPrinter p;
    b.plug(f);
    b.host->station(0)->attach_printer(&p);
    REQUIRE(b.run_until([&] { return b.host->station(0)->state() == StationState::Online; }));
    CHECK(b.host->station(0)->type_mismatch());
    CHECK(p.ready_count == 0);
}

TEST_CASE("recovery after no response and power cycle") {
    Bench b({display_at(0)});
    FakeStation f(0);
    TestDisplay d;
    b.plug(f);
    b.host->station(0)->attach_display(&d);
    REQUIRE(b.run_until([&] { return d.ready_count == 1; }));
    d.put(2, 0, "HELLO");
    REQUIRE(b.run_until([&] { return screen_matches(f, d.screen); }));

    // A couple of missed responses are retried without losing the station.
    f.drop_responses(2);
    b.run_for(100 * kMs);
    CHECK(d.lost_count == 0);
    CHECK(b.host->station(0)->state() == StationState::Online);

    f.set_powered(false);
    REQUIRE(b.run_until([&] { return d.lost_count == 1; }));
    CHECK(b.host->station(0)->state() == StationState::Absent);
    b.run_for(700 * kMs);
    f.set_powered(true);   // back with power-on status and an empty buffer
    REQUIRE(b.run_until([&] { return d.ready_count == 2; }));
    REQUIRE(b.run_until([&] { return screen_matches(f, d.screen); }));
    CHECK(f.cell(2, 0) == ebcdic('H'));
}

TEST_CASE("exception and line parity recovery repaint the screen") {
    Bench b({display_at(0)});
    FakeStation f(0);
    TestDisplay d;
    b.plug(f);
    b.host->station(0)->attach_display(&d);
    REQUIRE(b.run_until([&] { return d.ready_count == 1; }));
    d.put(4, 4, "DATA");
    REQUIRE(b.run_until([&] { return screen_matches(f, d.screen); }));

    f.latch_exception(0x0A);   // overrun reported in the next poll
    REQUIRE(b.run_until([&] { return b.host->station(0)->counters().exceptions == 1; }));
    REQUIRE(b.run_until([&] { return f.exception() == 0 && screen_matches(f, d.screen); }));

    f.inject_parity_error();   // the next queue load is lost on the wire
    d.put(8, 0, "AFTER LPE");
    REQUIRE(b.run_until([&] { return b.host->station(0)->counters().parity_errors >= 1; }));
    REQUIRE(b.run_until([&] {
        return b.host->station(0)->state() == StationState::Online && screen_matches(f, d.screen);
    }));
    CHECK(f.cell(8, 0) == ebcdic('A'));
    CHECK(d.lost_count == 0);
}

TEST_CASE("a station reset while online is rediscovered") {
    Bench b({display_at(0)});
    FakeStation f(0);
    TestDisplay d;
    b.plug(f);
    b.host->station(0)->attach_display(&d);
    REQUIRE(b.run_until([&] { return d.ready_count == 1; }));
    d.put(0, 0, "X");
    REQUIRE(b.run_until([&] { return screen_matches(f, d.screen); }));
    f.power_on();   // e.g. the terminal's automatic reset
    REQUIRE(b.run_until([&] { return d.ready_count == 2; }));
    CHECK(d.lost_count == 1);
    REQUIRE(b.run_until([&] { return screen_matches(f, d.screen); }));
}

namespace {

// The smallest tn5250::DisplaySession: a shadow that echoes data keys and
// records functions and AIDs.
struct FakeEngine : tn5250::DisplaySession {
    tn5250::ScreenSnapshot shadow;
    uint64_t gen = 1;
    unsigned alarms = 0;
    unsigned pumps = 0;
    std::vector<tn5250::Key> keys;
    std::vector<tn5250::Aid> aids;

    FakeEngine() { shadow.cells.assign(24 * 80, tn5250::Cell()); }
    void pump() override { ++pumps; }
    int rows() const override { return shadow.rows; }
    int cols() const override { return shadow.cols; }
    const tn5250::Cell& cell(int r, int c) const override { return shadow.at(r, c); }
    int cursor_row() const override { return shadow.cursor_row; }
    int cursor_col() const override { return shadow.cursor_col; }
    tn5250::Indicators indicators() const override { return shadow.indicators; }
    uint64_t generation() const override { return gen; }
    tn5250::Rect take_dirty() override { return tn5250::Rect(); }
    unsigned take_alarms() override { const unsigned a = alarms; alarms = 0; return a; }
    void snapshot(tn5250::ScreenSnapshot& out) const override { out = shadow; out.generation = gen; }
    tn5250::KeyResult key_data(uint8_t e) override {
        shadow.cells[size_t(shadow.cursor_row * 80 + shadow.cursor_col)].ch = e;
        ++shadow.cursor_col;
        ++gen;
        return tn5250::KeyResult::Accepted;
    }
    tn5250::KeyResult key_function(tn5250::Key k) override { keys.push_back(k); return tn5250::KeyResult::Accepted; }
    tn5250::KeyResult key_aid(tn5250::Aid a) override { aids.push_back(a); return tn5250::KeyResult::Accepted; }
    void set_record_sink(RecordSink) override {}
};

} // namespace

TEST_CASE("tn5250 session adapter drives a station") {
    Bench b({display_at(0)});
    FakeStation f(0);
    FakeEngine e;
    Tn5250DisplayAdapter adapter(e);
    b.plug(f);
    b.host->station(0)->attach_display(&adapter);
    REQUIRE(b.run_until([&] { return adapter.device_ready(); }));
    CHECK(e.pumps > 0);

    e.shadow.cells[0].ch = 0xC8;
    e.shadow.cells[1].ch = 0xC9;
    e.shadow.cursor_row = 1;
    e.shadow.cursor_col = 3;
    e.shadow.indicators.input_inhibited = false;
    e.shadow.indicators.message_waiting = true;
    ++e.gen;
    REQUIRE(b.run_until([&] { return f.cell(0, 1) == 0xC9 && f.cursor() == 83 && f.indicators() == 0x80; }));

    f.press(0x1C);   // a
    f.press(0x0D);   // Tab -> Field Advance
    f.press(0x5F);   // Help
    f.press(0x07);   // F1
    REQUIRE(b.run_until([&] { return e.aids.size() == 1; }));
    CHECK(e.aids[0] == tn5250::Aid::F1);
    REQUIRE(e.keys.size() == 2);
    CHECK(e.keys[0] == tn5250::Key::FieldAdvance);
    CHECK(e.keys[1] == tn5250::Key::Help);
    REQUIRE(b.run_until([&] { return f.cell(1, 3) == 0x81; }));
}
