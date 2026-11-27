// SPDX-License-Identifier: GPL-3.0-or-later
// IBM 3483 InfoWindow II: a 3270 display on coax (CUT mode).
//
// Bring-up. Facts (roms/ROMS.md, docs/bringup-3483.md):
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
#include "models/coax_terminal.hpp"
#include "hw/i80186_peripherals.hpp"
#include "machine/machine.hpp"
#include "models/parts.hpp"

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
        // A logic unit with a VGA monitor (project owner), so colour only:
        // the monochrome coax terminal is the 3472, with its own tube. The
        // strap byte is the sense code the monitor presents. The firmware
        // also knows a monochrome monitor class (types 1/3, sense 51), which
        // stays reachable with --straps 51 but is drawn in colour.
        info_.variants = {
            {"3483-V", "colour", true, kSenseType2, "IBM-3279-2-E", {}, {}},
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
            "attributes; a cell without colour, highlight or character set takes its field's (observed: "
            "Customization's underscore samples); rows continue from the regen origin 0:1000 when their row-list "
            "entry has bit 7 set (hypothesis); the status row is 0:0A00, where the runtime copies the operator "
            "information area (observed; a fixed address is a hypothesis)",
            "display modes: columns, rows and cell lines from 58000/58006/5800A as the mode records at ROM 81A0C set "
            "them (observed: 24x80, 32x80, 43x80, 27x132 records; Customization switches 24/32/43 rows); 5800A is the "
            "line count (observed: POST's blank-screen check)",
            "cursor: 58010 row:column, 5801C bit 7 shown, bit 6 block, bit 5 blink (observed, also set through the "
            "coax control register), 58016 its lines counted from 1 (numbering a hypothesis); rule line 58012 with "
            "5801E bits 6/5 (observed), drawn dotted (hypothesis)",
            "highlighting: blink, reverse (glyph dark on the cell's colour, as in photos), underscore solid or "
            "dashed by 58016 bit 7 (observed: Customization's underscore style); Black-On-White sets 5801A bit 15, "
            "drawn black on white (hypothesis for the drawing); colours are the shared terminal set (owner's decision), "
            "the drive table 58040-6F only feeds POST's video sense",
            "monitor sense 44 (display type 2, colour class, the firmware's default for an unknown monitor) by default; "
            "--straps 51 gives type 1 (the monochrome class); the sense lines are pulled low by 5840E bits 12-15",
            "alarm: 58402 bit 7 with a level in bits 6-0, a click is a pulse of bit 10 (observed: svc_177, POST's beep; "
            "reset's 809B sounds it until the early checks pass); level as volume is a hypothesis",
            "serial channel B: the same PS/2-style port as A, for a mouse (observed: the firmware sends a type-2 "
            "device F3 28 E8 03 F4; the mouse reading is a hypothesis); nothing attached",
            "setup: two copies at 47400/47500 in battery-backed RAM; a change in Customization saved with F3 survives a "
            "reboot, and the next boot goes to the normal start-up, not Offline Setup (observed: tests "
            "rom_3483_save_setup/boot_saved); printer port registers not found (printing is planned for v0.5)",
            "keyboard: the 122-key keyboard on serial channel A (POST resets it, reads ID BF BF, selects set 3) "
            "(observed dialogue; the channel is a hypothesis)",
            "ROM 06G5525 (U24, AM27C400), linear at 80000-FFFFF; per-segment checksums 1234/5678/9ABC/DEF0 "
            "(observed)",
            "CPU: IBM 39G8873, an 80186-core ASIC (observed: marking, PCB relocation by the firmware); "
            "clock 10 MHz (hypothesis, oscillator not read)",
            "RAM: 256 KiB DRAM at 00000 and 64 KiB battery-backed SRAM at 40000 (hypothesis; parts observed "
            "on the board, placement not)",
            "line: coax, CUT mode (hypothesis, strong: hardware address counters, no data-stream parser, "
            "oec drives a 3483-V); em34xx's CUT controller (host/coax/cut_controller.hpp) drives it over TN3270 or "
            "TN3270E; it announces feature 79, an extended attribute buffer with colour (observed)",
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
        build_coax_terminal(m, info_, CoaxBoardLayout{}, &cpu_io, sense);
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
