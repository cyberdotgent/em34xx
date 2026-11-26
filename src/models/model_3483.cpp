// SPDX-License-Identifier: GPL-3.0-or-later
// IBM 3483 InfoWindow II: a 3270 display on coax (CUT mode).
//
// Bring-up skeleton. Facts (roms/ROMS.md):
//  - one 512 KiB image (06G5525, AMD AM27C400 at U24) mapped linearly at
//    80000-FFFFF; reset vector jumps to C000:8DFC; every 64 KiB segment
//    sums to its own target (1234/5678/9ABC/DEF0, twice) [observed];
//  - CPU: IBM 39G8873, an ASIC with an 80186 core [observed: chip marking
//    "(C) Intel '78 '82"; the firmware moves the 80186 peripheral control
//    block to memory 8FF00 with OUT FFFE,18FF];
//  - devices at 58000 (display), 58100 (coax adapter, memory engine and
//    interrupt status), 58400 (system control), with display row lists in
//    block RAM at 50108/50508 [observed code; roles are hypotheses]; see
//    hw/board_3483.hpp. POST passes its RAM, window, coax wrap and display
//    tests and stops at the serial channel A test (12);
//  - RAM: two 256Kx4 DRAMs and two 32Kx8 SRAMs next to the battery
//    [observed, board photo]. Placing the DRAM at 00000-3FFFF and the
//    battery-backed SRAM at 40000-4FFFF is a hypothesis (the firmware's
//    data segment is 2000, and its setup block is near 47C00).
#include "models/model.hpp"

#include "bus/bus.hpp"
#include "hw/board_3483.hpp"
#include "hw/i80186_peripherals.hpp"
#include "machine/machine.hpp"
#include "models/parts.hpp"

namespace em34xx {

namespace {

constexpr uint32_t kDramBase = 0x00000, kDramSize = 0x40000;   // [hypothesis]
constexpr uint32_t kSramBase = 0x40000, kSramSize = 0x10000;   // battery-backed [hypothesis]
// Monitor sense code: type 1 (51), an 80-column 3270 tube [observed table;
// which code the 3483's own tube gives is a hypothesis].
constexpr uint8_t kMonitorSense = 0x51;

// Owns the board state the three register devices share; a device only so
// the machine keeps it alive.
struct BoardHolder : Device {
    BoardHolder(Bus& bus, I80186Peripherals& cpu_io, uint64_t clock_hz, uint8_t sense)
        : Device("board-3483"), board(bus, cpu_io, clock_hz, sense) {}
    Board3483 board;
};

class Model3483 : public Model {
public:
    Model3483() {
        info_.name = "3483";
        info_.description = "IBM 3483 InfoWindow II (3270 coax)";
        info_.status = ModelStatus::Bringup;
        info_.cpu = CpuLevel::I80186;
        info_.address_bits = 20;
        // The oscillator next to the CPU (Y49) is not legible in the photo;
        // 10 MHz is a pacing placeholder [hypothesis].
        info_.clock_hz = 10000000;
        info_.line = LineKind::Coax;
        info_.max_sessions = 1;
        info_.max_printer_sessions = 0;
        // The ROM header names "3483 V00"; oec was tested with a 3483-V
        // (documented, oec README). Monochrome or colour is not known yet.
        info_.variants = {
            {"3483-V", "display", false, 0x00, "", {phosphor_green()}, {}},
        };
        info_.roms = {
            {"system ROM (80000-FFFFF, reset vector)", "06g5525.bin", 0x80000,
             "da14974c16cb370ff6f505d69d4c2e5f84770bcaeae2ca5d6fda4e69413d23db", 0x80000},
        };
        info_.notes = {
            "bring-up: POST passes tests 3 (RAM), 4 (40000 window), 5 (coax internal wrap), 7 (display timing, "
            "video sense, memory engine) and 11, and stops at test 12 (serial channel A interrupt path) (observed)",
            "board devices (hw/board_3483.hpp) follow the firmware's use of them; display row lists, video sense, "
            "the coax reply words and the memory engine are fitted to POST's checks (hypothesis)",
            "font: compressed records in the 3477 format (bias 0, 8 KiB each, two per font) (observed); how the "
            "character generator holds them is not known, so the picture shows no readable text yet",
            "ROM 06G5525 (U24, AM27C400), linear at 80000-FFFFF; per-segment checksums 1234/5678/9ABC/DEF0 "
            "(observed)",
            "CPU: IBM 39G8873, an 80186-core ASIC (observed: marking, PCB relocation by the firmware); "
            "clock 10 MHz (hypothesis, oscillator not read)",
            "RAM: 256 KiB DRAM at 00000 and 64 KiB battery-backed SRAM at 40000 (hypothesis; parts observed "
            "on the board, placement not)",
            "line: coax, CUT mode (hypothesis, strong: hardware address counters, no data-stream parser, "
            "oec drives a 3483-V); no coax controller yet",
        };
    }

    const ModelInfo& info() const override { return info_; }

    void build(Machine& m, std::vector<LoadedRom>& roms) const override {
        Bus& bus = m.bus();
        bus.map_ram("DRAM", kDramBase, kDramSize);
        parts::battery_backed_ram(m, kSramBase, kSramSize);
        I80186Peripherals& cpu_io = m.add_device<I80186Peripherals>(bus);
        bus.set_interrupt_controller(&cpu_io);
        bus.map_ram("block RAM", 0x50000, 0x800);   // 50000-507FF: four blocks with registers at x108 [observed use]
        auto& board = m.add_device<BoardHolder>(bus, cpu_io, info_.clock_hz, kMonitorSense);
        Board3483::Display& disp = m.add_device<Board3483::Display>(board.board);
        Board3483::Coax& coax = m.add_device<Board3483::Coax>(board.board);
        Board3483::System& sys = m.add_device<Board3483::System>(board.board);
        m.add_device<Board3483::Window>(board.board);
        bus.map_mmio("display", 0x58000, 0x100, &disp);
        bus.map_mmio("coax", 0x58100, 0x100, &coax);
        bus.map_mmio("system", 0x58400, 0x100, &sys);
        bus.observe_writes(0x00000, 0x10000);
        bus.observe_writes(0x40000, 0x10000);
        bus.observe_writes(0x58000, 0x100);
        m.set_display(&disp);
        for (LoadedRom& r : roms) bus.map_rom(r.spec.file, r.spec.base, std::move(r.data));
    }

private:
    ModelInfo info_;
};

} // namespace

const Model& model_3483() {
    static const Model3483 m;
    return m;
}

} // namespace em34xx
