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
//    hw/board_3483.hpp. POST runs to its setup check (test 10) and
//    shows its NVR line readably;
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
#include "input/host_keymap.hpp"
#include "input/keyboard_ids.hpp"
#include "input/set3_keyboard.hpp"

#include <memory>
#include <stdexcept>

namespace em34xx {

namespace {

constexpr uint32_t kDramBase = 0x00000, kDramSize = 0x40000;   // [hypothesis]
constexpr uint32_t kSramBase = 0x40000, kSramSize = 0x10000;   // battery-backed [hypothesis]
// Monitor sense codes [observed: table C000:B9EB and the class table at
// C000:B9D7]: 44 = display type 2, class 2 (the colour drive table), which
// is also what the firmware assumes for an unknown monitor; 51 = type 1,
// class 1 (the monochrome table). Which code the 3483's own tube gives is
// a hypothesis; --straps selects another.
constexpr uint8_t kSenseType2 = 0x44;

// Owns the board state the three register devices share and the keyboard;
// a device only so the machine keeps them alive.
struct BoardHolder : Device {
    BoardHolder(Bus& bus, I80186Peripherals& cpu_io, uint64_t clock_hz, uint8_t sense)
        : Device("board-3483"), board(bus, cpu_io, clock_hz, sense) {}
    Board3483 board;
    std::unique_ptr<Set3Keyboard> keyboard;
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
        // The variant's strap byte is the monitor sense code it presents.
        info_.variants = {
            {"3483-V", "colour (display type 2)", true, kSenseType2, "", {}, {}},
        };
        info_.roms = {
            {"system ROM (80000-FFFFF, reset vector)", "06g5525.bin", 0x80000,
             "da14974c16cb370ff6f505d69d4c2e5f84770bcaeae2ca5d6fda4e69413d23db", 0x80000},
        };
        info_.notes = {
            "bring-up: POST passes tests 3 (RAM), 4 (40000 window), 5 (coax internal wrap), 7 (display timing, "
            "video sense, memory engine), 11-17 (serial channels, keyboard handshake) and 9, and shows its NVR line "
            "(no saved setup) in test 10; Reset continues, the firmware rewrites its setup blocks in SRAM, and the "
            "runtime shows Offline Setup (observed); with --nvram the repaired setup survives the next boot",
            "board devices (hw/board_3483.hpp) follow the firmware's use of them; display row lists, video sense, "
            "the coax reply words and the memory engine are fitted to POST's checks (hypothesis)",
            "character generator: the firmware decompresses its character sets into SRAM, 32 bytes per character: "
            "base set 48000, APL/GE set 4A000, companion planes 40000/42000 (observed); the companion byte as dots 9+ "
            "is a hypothesis",
            "display: cells are a 3270 buffer-code character and a 3270 extended attribute; C0-FF are field "
            "attributes; colours go through the firmware's own drive table at 58040-6F (one block per gun); 18-line "
            "cells (POST's blank test screen requires it); rows continue from the regen origin 0:1000 when their "
            "row-list entry has bit 7 set (hypotheses fitted to POST's video sense and the setup screens)",
            "monitor sense 44 (display type 2, colour class, the firmware's default for an unknown monitor) by default; "
            "--straps 51 gives type 1 (monochrome class); the sense lines are pulled low by 5840E bits 12-15",
            "keyboard: the 122-key keyboard on serial channel A (POST resets it, reads ID BF BF, selects set 3) "
            "(observed dialogue; the channel is a hypothesis)",
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
        const TerminalOptions& opt = m.options();
        const ModelVariant& variant = variant_for(opt);
        const uint8_t sense = opt.straps >= 0 ? uint8_t(opt.straps) : variant.straps;
        auto& board = m.add_device<BoardHolder>(bus, cpu_io, info_.clock_hz, sense);
        Board3483::Display& disp = m.add_device<Board3483::Display>(board.board);
        disp.set_colour(variant.colour);
        Board3483::Coax& coax = m.add_device<Board3483::Coax>(board.board);
        Board3483::System& sys = m.add_device<Board3483::System>(board.board);
        m.add_device<Board3483::Window>(board.board);

        // The 122-key keyboard on serial channel A [hypothesis: POST
        // expects ID BF BF and talks to channel A in its keyboard test].
        uint8_t id1 = 0xBF, id2 = 0xBF;
        std::string error;
        if (!resolve_keyboard_id(opt, id1, id2, error)) throw std::runtime_error(error);
        KeyboardType type = KeyboardType::Terminal122;
        parse_keyboard_type(opt.keyboard_type, type);
        board.keyboard = std::make_unique<Set3Keyboard>(type, id1, id2);
        board.keyboard->set_shift_fkeys(opt.shift_fkeys);
        auto keymap = std::make_shared<Keymap>();
        if (!load_keymap(opt.keymap, opt.shift_fkeys, *keymap, error)) throw std::runtime_error(error);
        board.keyboard->set_keymap(std::move(keymap));
        sys.attach_keyboard(board.keyboard.get(), info_.clock_hz);
        m.set_keyboard(board.keyboard.get());
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
