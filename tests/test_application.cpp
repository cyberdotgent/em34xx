// SPDX-License-Identifier: GPL-3.0-or-later
// Application runtime services for GUI front ends: power-cycle before
// connecting, and changing the display subtype while running (picture only,
// or a rebuilt and power-cycled machine).
#include "app/application.hpp"
#include "app/host_link.hpp"
#include "config/connect_args.hpp"
#include "machine/emulator.hpp"
#include "machine/model_info.hpp"
#include "models/model.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace em34xx;
using Args = std::vector<std::string>;

namespace {

class StubFrontend : public Frontend {
public:
    const char* name() const override { return "stub"; }
    bool run(Application& app, const FrontendOptions&, std::string&) override {
        app.request_quit(0);
        return true;
    }
    void request_quit() override {}
    void set_title(const std::string&) override {}
    void set_status(const std::string&) override {}
};

bool roms_present() {
    const std::string dir = std::string(EM34XX_SOURCE_DIR) + "/roms/";
    return std::filesystem::exists(dir + "95f4171.bin") && std::filesystem::exists(dir + "95f4172.bin");
}

uint64_t instructions(Application& app) {
    return app.emulator().with_machine([](Machine& m) { return m.cpu().instructions(); });
}

// Let the emulator run until it has executed at least n instructions.
void run_at_least(Application& app, uint64_t n) {
    for (int i = 0; i < 2000 && instructions(app) < n; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

struct RunningApp {
    std::string nvram;
    Application app{std::make_unique<StubFrontend>(), nullptr};
    explicit RunningApp(const char* display) {
        nvram = (std::filesystem::temp_directory_path() / "em34xx-test-app.nvram").string();
        std::remove(nvram.c_str());
        const std::string roms = std::string(EM34XX_SOURCE_DIR) + "/roms";
        std::vector<std::string> args = {"em34xx",  "--model", "3477",  "--rom-dir", roms,
                                         "--speed", "0",       "--nvram", nvram,      "--display",
                                         display,   "--log",   "warn"};
        std::vector<char*> argv;
        for (std::string& a : args) argv.push_back(&a[0]);
        argv.push_back(nullptr);
        ok = app.start(int(args.size()), argv.data());
    }
    ~RunningApp() {
        app.finish();
        std::remove(nvram.c_str());
    }
    bool ok = false;
};

} // namespace

TEST_CASE("machine: the line host must be for the model's line") {
    static ModelInfo twinax, coax;
    twinax.cpu = coax.cpu = CpuLevel::I8088;
    twinax.address_bits = coax.address_bits = 20;
    twinax.line = LineKind::Twinax;
    coax.line = LineKind::Coax;

    Machine t(twinax);
    REQUIRE(t.twinax_host());
    CHECK(std::string(t.line_host()->kind()) == "none");
    CHECK(t.line_host()->line() == LineKind::Twinax);
    CHECK_FALSE(t.set_line_host(std::make_unique<NullLineHost>(LineKind::Coax)));
    CHECK(t.set_line_host(std::make_unique<NullTwinaxHost>()));

    // A terminal on another line has no Twinax interface, and a Twinax
    // controller offered to it is handed back unused.
    Machine c(coax);
    CHECK(c.twinax_host() == nullptr);
    REQUIRE(c.line_host());
    CHECK(c.line_host()->line() == LineKind::Coax);
    CHECK_FALSE(c.set_line_host(std::make_unique<NullTwinaxHost>()));
    std::unique_ptr<LineHost> back = c.swap_line_host(std::make_unique<NullTwinaxHost>());
    REQUIRE(back);
    CHECK(back->line() == LineKind::Twinax);
    CHECK(c.line_host()->line() == LineKind::Coax);
}

TEST_CASE("emulator: replace_machine swaps a stopped machine and reports a new picture") {
    static ModelInfo info;
    info.name = "test";
    info.cpu = CpuLevel::I8088;
    info.address_bits = 20;
    auto first = std::make_unique<Machine>(info);
    first->bus().map_ram("ram", 0, 0x100000);
    Machine* first_ptr = first.get();
    Emulator emu(std::move(first));
    emu.start();
    emu.resume(1000);
    emu.wait_stopped();
    auto second = std::make_unique<Machine>(info);
    second->bus().map_ram("ram", 0, 0x100000);
    second->reset();
    Machine* second_ptr = second.get();
    const uint64_t gen = emu.display_generation();
    std::unique_ptr<Machine> old = emu.replace_machine(std::move(second));
    CHECK(old.get() == first_ptr);
    CHECK(emu.with_machine([](Machine& m) { return &m; }) == second_ptr);
    CHECK(emu.display_generation() != gen);
    CHECK(emu.last_stop().reason == StopEvent::Reason::Reset);
    emu.shutdown();
}

TEST_CASE("application: the phosphor changes the picture only, without a reset") {
    if (!roms_present()) return;   // the ROM images are never committed
    RunningApp r("green");
    REQUIRE(r.ok);
    Application& app = r.app;
    CHECK(app.variant().id == "3477-FG");
    CHECK(app.config().terminal.phosphor == "green");
    run_at_least(app, 200000);
    const uint64_t before = instructions(app);
    std::string error;
    // Paper-white too: it is a tube of the same unit, not other straps.
    for (const Phosphor* p : {&phosphor_amber(), &phosphor_paper_white()}) {
        REQUIRE(app.set_phosphor(p->id, error));
        CHECK(instructions(app) >= before);   // the same machine, still running
        CHECK(app.emulator().with_machine([](Machine& m) { return m.tube().phosphor; }) == p->rgb);
        CHECK_FALSE(app.emulator().with_machine([](Machine& m) { return m.tube().colour; }));
        CHECK(app.config().terminal.phosphor == p->id);
        CHECK(app.emulator().with_machine([](Machine& m) { return m.bus().peek8(0x2000A); }) == 0x0C);
    }
    CHECK(app.powered());
    CHECK_FALSE(app.set_phosphor("magenta", error));
    CHECK(app.config().terminal.phosphor == "paperwhite");
}

TEST_CASE("application: another variant rebuilds and power-cycles; new sessions announce it") {
    if (!roms_present()) return;
    RunningApp r("amber");
    REQUIRE(r.ok);
    Application& app = r.app;
    // A session to a port nobody listens on: connect() succeeds, the
    // session retries in the background.
    std::string message, error;
    REQUIRE(app.host_link().connect(Args{"connect", "127.0.0.1:1"}, message, error));
    CHECK(app.host_link().status().find("IBM-3477-FG") != std::string::npos);
    run_at_least(app, 400000);
    const uint64_t before = instructions(app);
    TerminalOptions options = app.config().terminal;
    options.variant = "3477-FC";
    REQUIRE(app.set_model(app.model(), options, error));
    CHECK(instructions(app) < before);   // a new machine, from the reset vector
    CHECK(app.host_link().controller_kind() == "none");   // the sessions were closed
    CHECK_FALSE(app.host_link().bridging());
    CHECK(app.variant().id == "3477-FC");
    CHECK(app.config().terminal.phosphor.empty());
    CHECK(app.emulator().with_machine([](Machine& m) { return m.tube().colour; }));
    CHECK(app.powered());
    // POST runs on the new machine.
    run_at_least(app, 200000);
    CHECK(instructions(app) >= 200000);
    // The reconnect announces the colour device type.
    REQUIRE(app.host_link().connect(Args{"connect", "127.0.0.1:1"}, message, error));
    CHECK(app.host_link().status().find("IBM-3477-FC") != std::string::npos);
    // Back to the monochrome unit: its first phosphor.
    options.variant = "3477-FG";
    REQUIRE(app.set_model(app.model(), options, error));
    CHECK(app.variant().id == "3477-FG");
    CHECK(app.config().terminal.phosphor == "amber");
    CHECK_FALSE(app.emulator().with_machine([](Machine& m) { return m.tube().colour; }));
}

TEST_CASE("application: connect after a power cycle resets the terminal, then connects") {
    if (!roms_present()) return;
    RunningApp r("green");
    REQUIRE(r.ok);
    Application& app = r.app;
    std::string message, error;
    REQUIRE(app.host_link().connect(Args{"connect", "127.0.0.1:1"}, message, error));
    run_at_least(app, 400000);
    const uint64_t resets = app.emulator().with_machine([](Machine& m) { return m.reset_count(); });
    AppConfig cfg;
    REQUIRE(parse_connect_args({"connect", "127.0.0.1:2"}, app.host_link().base(), cfg, error));
    REQUIRE(app.connect_after_power_cycle(cfg, message, error));
    // Reset: POST starts again.
    CHECK(app.emulator().with_machine([](Machine& m) { return m.reset_count(); }) == resets + 1);
    CHECK(message.find("previous session closed") == std::string::npos);   // closed before the reset
    CHECK(app.host_link().controller_kind() == "tn5250");
    CHECK(app.host_link().bridging());
    CHECK(app.host_link().status().find("127.0.0.1:2") != std::string::npos);
    CHECK(app.powered());
}

TEST_CASE("application: the host connection follows the terminal's reset and power") {
    if (!roms_present()) return;
    RunningApp r("green");
    REQUIRE(r.ok);
    Application& app = r.app;
    std::string message, error;
    // Never connected: a reset does not put a controller on the line.
    app.reset();
    CHECK(app.host_link().controller_kind() == "none");
    CHECK_FALSE(app.host_link().reconnect(message, error));

    REQUIRE(app.host_link().connect(Args{"connect", "127.0.0.1:1"}, message, error));
    CHECK(app.host_link().controller_kind() == "tn5250");
    // Disconnected by the user: no controller, until the terminal is reset.
    REQUIRE(app.host_link().disconnect(error));
    CHECK(app.host_link().controller_kind() == "none");
    app.reset();
    CHECK(app.host_link().controller_kind() == "tn5250");
    CHECK(app.host_link().status().find("127.0.0.1:1") != std::string::npos);
    CHECK(app.powered());
    // The same at power-up; while it is off nothing is dialled.
    REQUIRE(app.host_link().disconnect(error));
    app.power_off();
    CHECK(app.host_link().controller_kind() == "none");
    app.power_on();
    CHECK(app.host_link().controller_kind() == "tn5250");
    // A bridge already on the line is left alone.
    CHECK(app.host_link().reconnect(message, error));
}
