// SPDX-License-Identifier: GPL-3.0-or-later
// IBM 3472 InfoWindow: a 3270 display on coax (CUT mode) with its own
// monochrome tube.
//
// Bring-up skeleton. Facts (roms/ROMS.md, analysis notes):
//  - two NEC mask ROMs of 128 KiB, 09F4604 on the even and 09F4605 on the
//    odd byte lane of a 16-bit bus, together one 256 KiB image at
//    C0000-FFFFF; reset vector CLI; JMP E000:0FBE; per-segment checksums
//    1234/5678/9ABC/DEF0 [observed];
//  - CPU: AMD N8086-2 [observed, board photo]; no interrupt controller of
//    its own: the gate array presents vector 20h [hypothesis, strong: the
//    firmware hooks only 20h and sends no EOI];
//  - the same register design as the 3483 at other addresses [observed in
//    code]: display 28000, coax adapter and interrupt status 28100, system
//    control 30000, row lists and coax status block 20108/20308/20508;
//    modelled by the 3483's board model (hw/board_3483.hpp);
//  - gate array: Toshiba 09F4534 (T3A17B) [observed, board photo];
//  - RAM: 64 KiB DRAM at 00000 (two Panasonic MN41464A-08, 64Kx4), 64 KiB
//    SRAM at 10000-1FFFF (two NEC D43256AGU-10LL, 32Kx8, low-power parts
//    next to a Sanyo lithium cell) [parts observed, board photo]; the
//    character generator at 10000-1BFFF and the setup above it [observed
//    use]; the SRAM as the battery-backed part [hypothesis, strong: the
//    firmware keeps its setup there, and only those parts are low-power
//    ones beside the cell]; gate array
//    RAM 20000-207FF [observed use; placement from the RAM tests].
#include "models/model.hpp"

#include "bus/bus.hpp"
#include "machine/machine.hpp"
#include "models/coax_terminal.hpp"
#include "models/parts.hpp"

#include <stdexcept>

namespace em34xx {

namespace {

constexpr uint32_t kDramBase = 0x00000, kDramSize = 0x10000;
constexpr uint32_t kSramBase = 0x10000, kSramSize = 0x10000;
constexpr uint32_t kBlockRamBase = 0x20000, kBlockRamSize = 0x800;
constexpr uint32_t kRomBase = 0xC0000, kChipSize = 0x20000;
// The 3483's board at the 3472's addresses [observed: the firmware's first
// accesses write the same offsets of 28000, 28100 and 30000 that the
// 3483's reset writes of 58000, 58100 and 58400; its row lists carry a
// 3D00h cell bias (E000:38EE), so the regen buffer starts at 0:7A00, where
// POST writes its error line]. Vertical sync reads high during the pulse
// [observed: POST test 6 times the high phase at 14-39 loops and the low
// phase at 756-2265]. No colour drive table: monochrome.
CoaxBoardLayout layout_3472() {
    CoaxBoardLayout l;
    l.display = 0x28000;
    l.coax = 0x28100;
    l.system = 0x30000;
    l.sram = kSramBase;
    l.block_ram = kBlockRamBase;
    l.regen = 0x7A00;
    // The status row [observed: the status routine (E000:A8AE) builds the
    // operator information area from coax cells 0-79 and copies 84h cells
    // to 0:96F8 (E000:AA01); the runtime's row list names the same cell
    // (4B7C) in its entries below the screen, as the 3483's names 0:0A00].
    l.status_row = 0x96F8;
    l.sync_reads_high = true;
    l.monochrome = true;
    l.cell_dots = 12;   // main 3-0, companion 7-0 [observed: its fonts]
    l.dot_low_bit = 0;
    l.monitor_sense_lines = false;   // its own tube: a display ID instead
    return l;
}

// Display ID, bits 7-5 of the system sense register: 000, display type 1
// [observed: the firmware's type 1 runs the video test's subtests that pass
// and goes on line; hypothesis: that a real board gives 000]. --straps 20
// gives 001 (type 1 with another gate array setting), 40 gives 01x (type 2).
constexpr uint8_t kDisplayId = 0x00;

class Model3472 : public Model {
public:
    Model3472() {
        info_.name = "3472";
        info_.description = "IBM 3472 InfoWindow (3270 coax)";
        info_.status = ModelStatus::Bringup;
        info_.cpu = CpuLevel::I8086;
        info_.address_bits = 20;
        // Oscillators 56.6092 and 45.9600 MHz on the board [observed]; the
        // CPU clock is not known: 8 MHz (N8086-2 grade) as a pacing
        // placeholder [hypothesis].
        info_.clock_hz = 8000000;
        info_.line = LineKind::Coax;
        info_.max_sessions = 1;
        info_.max_printer_sessions = 0;
        // A monochrome unit with its own tube, so the 5250 monochrome
        // units' phosphors.
        info_.variants = {
            {"3472-G", "monochrome", false, kDisplayId, "", {phosphor_green(), phosphor_amber(), phosphor_paper_white()},
             {}},
        };
        // Both chips load at the image's base; build() interleaves them.
        info_.roms = {
            {"system ROM, even bytes (C0000-FFFFF)", "09f4604.bin", kChipSize,
             "05547ffd2d69e0779c7d696fd425c4c60cf7716cd2e8e303dea5628b13d52ac5", kRomBase},
            {"system ROM, odd bytes (C0000-FFFFF)", "09f4605.bin", kChipSize,
             "8bea2740d1931e5879e13b48370640a5bb0076c43c26e435b324a6a6dc7b4bb5", kRomBase},
        };
        info_.notes = {
            "bring-up: the 3483's board model (hw/board_3483.hpp) at 28000 (display), 28100 (coax), 30000 "
            "(system), SRAM 10000, gate array RAM 20000 (observed: same register offsets in the firmware's reset); "
            "the gate array supplies interrupt vector 20h (hypothesis)",
            "ROMs 09F4604 (even bytes) and 09F4605 (odd bytes), 128 KiB each, interleaved at C0000-FFFFF "
            "(observed: checksums, reset vector)",
            "CPU: AMD N8086-2 (observed); clock 8 MHz (hypothesis; oscillators 56.6092 and 45.9600 MHz observed)",
            "RAM: 64 KiB DRAM (2x MN41464A-08) at 00000 and 64 KiB SRAM (2x NEC D43256AGU-10LL) at 10000, the SRAM "
            "battery-backed by a lithium cell (parts observed on the board; placement observed in use, the "
            "battery-backed part a hypothesis); a first boot with an empty SRAM stops at M12, Reset writes the "
            "setup, and the next boot passes it (observed: tests rom_3472_save_setup/boot_saved)",
            "gate array: Toshiba 09F4534 (observed, board photo)",
            "no monitor sense lines: the firmware reads only bits 7-0 of 3000E; bits 7-5 are a display ID, 000/001 "
            "type 1, 01x type 2, 1xx none (observed, E000:13AB); --straps sets it, 00 by default (hypothesis); types "
            "1 and 2 both pass POST (observed)",
            "printer port: a DB-25 parallel port for the printer session (documented: project owner); its registers "
            "are not found and it is not modelled",
            "line: coax, CUT mode (hypothesis, strong; same adapter design as the 3483)",
        };
    }

    const ModelInfo& info() const override { return info_; }

    void build(Machine& m, std::vector<LoadedRom>& roms) const override {
        Bus& bus = m.bus();
        bus.map_ram("DRAM", kDramBase, kDramSize);
        parts::battery_backed_ram(m, kSramBase, kSramSize);
        bus.map_ram("gate array RAM", kBlockRamBase, kBlockRamSize);
        const TerminalOptions& opt = m.options();
        const uint8_t sense = opt.straps >= 0 ? uint8_t(opt.straps) : variant_for(opt).straps;
        build_coax_terminal(m, info_, layout_3472(), nullptr, sense);
        if (roms.size() != 2 || roms[0].data.size() != kChipSize || roms[1].data.size() != kChipSize)
            throw std::runtime_error("3472: needs both ROM chips");
        std::vector<uint8_t> image(2 * kChipSize);
        for (uint32_t i = 0; i < kChipSize; ++i) {
            image[2 * i] = roms[0].data[i];       // 09F4604: even bytes
            image[2 * i + 1] = roms[1].data[i];   // 09F4605: odd bytes
        }
        bus.map_rom("09f4604.bin + 09f4605.bin", kRomBase, std::move(image));
    }

private:
    ModelInfo info_;
};

} // namespace

const Model& model_3472() {
    static const Model3472 m;
    return m;
}

} // namespace em34xx
