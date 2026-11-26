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
//    those devices are not modelled yet, and stop the machine on first
//    access;
//  - RAM: 64 KiB DRAM at 00000, SRAM at 10000-1FFFF (character generator
//    10000-1BFFF, setup 1C000-1FFFF, battery-backed [hypothesis]), gate
//    array RAM 20000-207FF [observed use; placement from the RAM tests].
#include "models/model.hpp"

#include "bus/bus.hpp"
#include "machine/machine.hpp"
#include "models/parts.hpp"

#include <stdexcept>

namespace em34xx {

namespace {

constexpr uint32_t kDramBase = 0x00000, kDramSize = 0x10000;
constexpr uint32_t kSramBase = 0x10000, kSramSize = 0x10000;
constexpr uint32_t kBlockRamBase = 0x20000, kBlockRamSize = 0x800;
constexpr uint32_t kRomBase = 0xC0000, kChipSize = 0x20000;

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
            {"3472-G", "monochrome", false, 0x00, "", {phosphor_green(), phosphor_amber(), phosphor_paper_white()},
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
            "bring-up skeleton: ROM, RAM and the 8086 only; the board's devices (28000, 28100, 30000) are not "
            "modelled and stop the machine on first access",
            "ROMs 09F4604 (even bytes) and 09F4605 (odd bytes), 128 KiB each, interleaved at C0000-FFFFF "
            "(observed: checksums, reset vector)",
            "CPU: AMD N8086-2 (observed); clock 8 MHz (hypothesis)",
            "line: coax, CUT mode (hypothesis, strong; same adapter design as the 3483)",
        };
    }

    const ModelInfo& info() const override { return info_; }

    void build(Machine& m, std::vector<LoadedRom>& roms) const override {
        Bus& bus = m.bus();
        bus.map_ram("DRAM", kDramBase, kDramSize);
        parts::battery_backed_ram(m, kSramBase, kSramSize);
        bus.map_ram("gate array RAM", kBlockRamBase, kBlockRamSize);
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
