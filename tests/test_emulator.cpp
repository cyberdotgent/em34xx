// SPDX-License-Identifier: GPL-3.0-or-later
// Emulator run control services used by front ends: display change
// notification.
#include "machine/emulator.hpp"
#include "machine/model_info.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <thread>

using namespace tn34xx;

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
