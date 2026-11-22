// SPDX-License-Identifier: GPL-3.0-or-later
// Emulator run control services used by front ends: display change
// notification.
#include "machine/emulator.hpp"
#include "machine/model_info.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace em34xx;

namespace {

ModelInfo test_model() {
    ModelInfo m;
    m.name = "test";
    m.cpu = CpuLevel::I8088;
    m.address_bits = 20;
    return m;
}

class PhaseDisplay : public DisplaySource {
public:
    void render(Framebuffer& fb) override { fb.resize(8, 8); }
    uint64_t animation_phase() const override { return phase; }
    uint64_t phase = 0;
};

} // namespace

TEST_CASE("bus counts writes into observed pages only") {
    Bus bus(20);
    bus.map_ram("ram", 0, 0x20000);
    bus.observe_writes(0x8000, 0x100);
    bus.mem_write8(0x7FFF, 1);
    CHECK(bus.observed_writes() == 0);
    bus.mem_write8(0x8000, 1);
    bus.mem_write8(0x80FF, 1);
    CHECK(bus.observed_writes() == 2);
    bus.mem_write8(0x8100, 1);
    CHECK(bus.observed_writes() == 2);
    CHECK(bus.poke8(0x8010, 2));   // debugger writes count too
    CHECK(bus.observed_writes() == 3);
}

TEST_CASE("display generation follows observed writes and the animation phase") {
    static const ModelInfo info = test_model();
    auto machine = std::make_unique<Machine>(info);
    machine->bus().map_ram("ram", 0, 0x20000);
    machine->bus().observe_writes(0x8000, 0x1000);
    PhaseDisplay display;
    machine->set_display(&display);
    Emulator emu(std::move(machine));

    std::atomic<uint64_t> called{0};
    emu.set_display_callback([&](uint64_t gen) { called.store(gen); });

    // The first look after construction reports a change (nothing drawn yet).
    emu.with_machine([](Machine&) {});
    const uint64_t g0 = emu.display_generation();
    CHECK(g0 == 1);
    CHECK(called.load() == 1);

    // Unrelated RAM: no change.
    emu.with_machine([](Machine& m) { m.bus().poke8(0x100, 0x55); });
    CHECK(emu.display_generation() == g0);
    CHECK(emu.wait_display_change(g0, 1) == g0);

    // A write into the observed window.
    emu.with_machine([](Machine& m) { m.bus().poke8(0x8000, 0x55); });
    CHECK(emu.display_generation() == g0 + 1);

    // A new animation phase (blink) without any write.
    emu.with_machine([&](Machine&) { display.phase = 1; });
    CHECK(emu.display_generation() == g0 + 2);
    CHECK(called.load() == g0 + 2);

    // A waiter wakes when another thread publishes a change.
    const uint64_t seen = emu.display_generation();
    std::thread writer([&] { emu.with_machine([](Machine& m) { m.bus().poke8(0x8001, 1); }); });
    const uint64_t now = emu.wait_display_change(seen, 5000);
    writer.join();
    CHECK(now == seen + 1);

    // Quitting wakes waiters without a change.
    emu.request_quit(0);
    CHECK(emu.wait_display_change(emu.display_generation(), 5000) == emu.display_generation());
    emu.set_display_callback(nullptr);
}

TEST_CASE("a speed change does not stall the machine") {
    // RAM full of zeros is an endless run of ADD [BX+SI],AL.
    static const ModelInfo info = [] {
        ModelInfo m = test_model();
        m.clock_hz = 1000000;
        return m;
    }();
    auto machine = std::make_unique<Machine>(info);
    machine->bus().map_ram("ram", 0, 0x100000);
    Emulator emu(std::move(machine));
    const auto cycles = [&] { return emu.with_machine([](Machine& m) { return m.cpu().cycles(); }); };

    // Run unthrottled for a while: the machine gets far ahead of its clock
    // (also exercises the lock hand-over to with_machine() callers).
    emu.set_speed_percent(0);
    emu.start();
    emu.resume();
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300)) {
        cycles();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // At 100% it must carry on from here, not sleep until the clock has
    // caught up with everything run so far: 0.2 s of machine time, allowed
    // 3 s.
    emu.set_speed_percent(100);
    const uint64_t c0 = cycles();
    const auto t1 = std::chrono::steady_clock::now();
    uint64_t c1 = c0;
    while (c1 < c0 + 200000 && std::chrono::steady_clock::now() - t1 < std::chrono::seconds(3)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        c1 = cycles();
    }
    CHECK(c1 >= c0 + 200000);

    emu.request_stop();
    CHECK(emu.wait_stopped().reason == StopEvent::Reason::User);
}
