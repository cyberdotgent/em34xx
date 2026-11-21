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

TEST_CASE("3477: which display subtype changes need a power cycle") {
    const Model& m = model_3477();
    TerminalOptions o;
    o.display = DisplaySubtype::Green;
    CHECK_FALSE(m.display_change_needs_power_cycle(o, DisplaySubtype::Green));
    CHECK_FALSE(m.display_change_needs_power_cycle(o, DisplaySubtype::Amber));   // same straps: picture only
    CHECK(m.display_change_needs_power_cycle(o, DisplaySubtype::PaperWhite));    // straps 0C -> 0E
    CHECK(m.display_change_needs_power_cycle(o, DisplaySubtype::Color));         // device type
    o.display = DisplaySubtype::Color;
    CHECK(m.display_change_needs_power_cycle(o, DisplaySubtype::Amber));
    // Explicit straps do not follow the subtype.
    o.display = DisplaySubtype::Green;
    o.straps = 0x0C;
    CHECK_FALSE(m.display_change_needs_power_cycle(o, DisplaySubtype::PaperWhite));
    CHECK(m.display_change_needs_power_cycle(o, DisplaySubtype::Color));
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

TEST_CASE("application: green -> amber changes the picture only, without a reset") {
    if (!roms_present()) return;   // the ROM images are never committed
    RunningApp r("green");
    REQUIRE(r.ok);
    Application& app = r.app;
    run_at_least(app, 200000);
    CHECK_FALSE(app.display_change_needs_power_cycle(DisplaySubtype::Amber));
    const uint64_t before = instructions(app);
    std::string error;
    REQUIRE(app.set_display_subtype(DisplaySubtype::Amber, error));
    CHECK(instructions(app) >= before);   // the same machine, still running
    CHECK(app.emulator().with_machine([](Machine& m) { return m.options().display; }) == DisplaySubtype::Amber);
    CHECK(app.config().terminal.display == DisplaySubtype::Amber);
    CHECK(app.powered());
}

TEST_CASE("application: mono -> colour rebuilds and power-cycles; new sessions announce colour") {
    if (!roms_present()) return;
    RunningApp r("green");
    REQUIRE(r.ok);
    Application& app = r.app;
    // A session to a port nobody listens on: connect() succeeds, the
    // session retries in the background.
    std::string message, error;
    REQUIRE(app.host_link().connect(Args{"connect", "127.0.0.1:1"}, message, error));
    CHECK(app.host_link().status().find("IBM-3477-FG") != std::string::npos);
    run_at_least(app, 400000);
    const uint64_t before = instructions(app);
    REQUIRE(app.display_change_needs_power_cycle(DisplaySubtype::Color));
    REQUIRE(app.set_display_subtype(DisplaySubtype::Color, error));
    CHECK(instructions(app) < before);   // a new machine, from the reset vector
    CHECK(app.host_link().controller_kind() == "none");   // the sessions were closed
    CHECK(app.emulator().with_machine([](Machine& m) { return m.options().display; }) == DisplaySubtype::Color);
    CHECK(app.config().terminal.display == DisplaySubtype::Color);
    CHECK(app.powered());
    // POST runs on the new machine.
    run_at_least(app, 200000);
    CHECK(instructions(app) >= 200000);
    // The reconnect announces the colour device type.
    REQUIRE(app.host_link().connect(Args{"connect", "127.0.0.1:1"}, message, error));
    CHECK(app.host_link().status().find("IBM-3477-FC") != std::string::npos);
    // Back to monochrome; paper-white changes the straps, so it rebuilds too.
    REQUIRE(app.set_display_subtype(DisplaySubtype::PaperWhite, error));
    CHECK(app.emulator().with_machine([](Machine& m) { return m.bus().peek8(0x2000A); }) == 0x0E);
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
    CHECK(app.host_link().status().find("127.0.0.1:2") != std::string::npos);
    CHECK(app.powered());
}
