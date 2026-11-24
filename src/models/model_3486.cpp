// SPDX-License-Identifier: GPL-3.0-or-later
// IBM 3486 InfoWindow II.
//
// Runs the firmware image dumped from a 3487 logic module (54G0010, one
// 512 KiB chip; roms/ROMS.md). docs/bringup-3486.md gives the evidence for
// every entry. Summary:
//  - One 512 KiB image mapped at 80000-FFFFF, reset vector at the top
//    [observed: self-referencing pointer tables, reset vector, POST word
//    sum]. The same firmware identifies as 3486, 3487 or 3488 depending on
//    the straps [observed: VPD model table].
//  - The board is a 3477 derivative: system gate array at 20000 and display
//    controller at 20100, with the same interrupt, timer, keyboard, Twinax,
//    printer and I2C registers [observed: same register use]. It adds a
//    third Twinax station channel and a second serial port for a pointing
//    device [observed: POST tests both]; other new registers are plain
//    storage here.
//  - 20145 bit 7 reads 0 here, and the firmware then uses another
//    interrupt layout than the 3477's (keyboard on 0Fh, display on 0Dh, a
//    third Twinax channel) [observed code]; the model has that layout
//    [hypothesis for the board].
//  - RAM from 00000; the firmware uses addresses up to about 52000 [observed],
//    and 60000-7FFFF is an optional ROM checked only when a strap says it is
//    fitted [observed]. 384 KiB (00000-5FFFF) is a hypothesis.
//  - NEC V20 CPU; no 80186-only opcode in the reachable code, 8086 code
//    generation (repeated single-bit shifts), no port I/O [observed].
// The 3486's own chip, 54G0011, holds only half of its firmware. Loaded with
// --allow-bad-rom, its four 64 KiB blocks are mapped at the segments they
// were linked for and the four missing segments are left unmapped, so the
// first access to them stops the machine instead of running code from the
// wrong segment.
#include "models/model.hpp"

#include "models/alignment_shim_3486.hpp"
#include "models/board_3486.hpp"
#include "models/parts.hpp"
#include "machine/machine.hpp"
#include "util/log.hpp"

#include <stdexcept>

namespace em34xx {

namespace {

constexpr uint32_t kRamSize = 0x60000;        // 00000-5FFFF [hypothesis]
constexpr uint32_t kOptionRomBase = 0x60000;  // optional ROM, 60000-7FFFF [observed: checksum range]
constexpr uint32_t kOptionRomSize = 0x20000;

// Configuration straps (2000A) [observed decoder, hypothesis for the unit]:
//  bits 3-1 = 001: display type 1 or 2 (VPD "3486 BA0"; 2012F bit 1 picks
//  which), mono family; bit 6 = 1: no optional ROM at 60000.
// The other codes select the 3487 tubes (101 HG0/HA0, 100 HW0, 110 HC0
// colour) and the 3488 (010); 011 and 000/111 are rejected by POST.
constexpr uint8_t kDefaultStraps = 0x42;

// Straps per variant [observed decoder; which unit has which tube is a
// hypothesis]: the 3486 is type 2 (42), the monochrome 3487 types 3/4
// (HG0/HA0, 4A) and the colour 3487 type 6 (HC0, 4C). The firmware also
// knows a paper-white 3487 (HW0, type 5, 48) with its own video timing; no
// variant selects it, since the tube is a phosphor of the monochrome unit,
// and it stays reachable with --straps 48.
constexpr uint8_t kStraps3487Mono = 0x4A;
constexpr uint8_t kStraps3487Colour = 0x4C;

// The 3486 and the 3487 are one board and one firmware; they differ in the
// tube and therefore in their variants (straps). This is what they share:
// the board's facts and its build. Each terminal is a class of its own
// below.
class InfoWindow2Board : public Model {
protected:
    InfoWindow2Board(const char* name, const char* description, std::vector<ModelVariant> variants) {
        info_.name = name;
        info_.description = description;
        info_.variants = std::move(variants);
        info_.status = ModelStatus::Bringup;
        // NEC V20 (D70108L-8) on the logic board [observed].
        info_.cpu = CpuLevel::NecV20;
        info_.address_bits = 20;
        // 30.000 MHz master oscillator on the logic board (observed); the
        // CPU clock is assumed to be a quarter of it (hypothesis).
        info_.clock_hz = 7500000;
        info_.line = LineKind::Twinax;
        info_.max_sessions = 2;
        info_.max_printer_sessions = 1;
        info_.options = {
            {"vdo-shim", "TEMPORARY: stand in for the unmodelled video signature of the power-on display test "
                         "(off: POST stops at VDO)", true},
            {"alignment-shim", "TEMPORARY, colour units: mark the monitor alignment done in the EEPROM (off: the "
                               "unit shows its alignment pattern and stays off line)", true},
        };
        info_.roms = {
            {"system ROM (80000-FFFFF, reset vector)", "54g0010.bin", 0x80000,
             "7d04a011c1865c8c0745f9d2bb2948964dcd1ab1d0e8e4fa8cbde3d3b93ef51c", 0x80000},
        };
        info_.known_bad_roms = {
            {"", "f5d968694fdb3b6991b219460eb0ce5eec131797a9c0ea928ec61a6a5b9291ef",
             "54G0011, the 3486's 256 KiB chip: segments 8000/9000/C000/D000 are not in it"},
        };
        info_.notes = {
            "ROM 54G0010, dumped from a 3487 logic module as one 512 KiB chip (documented by the dumper); "
            "its word sum over 80000-FFFFF is 619A, which POST requires (observed)",
            "one firmware for 3486/3487/3488: the straps select the model in the VPD (observed)",
            "the 3486's own chip, 54G0011, holds segments A000/B000/E000/F000 only and at another "
            "firmware level (observed: different word sum); it runs only as far as the first call into "
            "segment D000 and needs --allow-bad-rom",
            "20145 bit 7 selects the interrupt layout (observed code): 1 = keyboard on vector 0Eh as on "
            "the 3477, 0 = keyboard on 0Fh, second serial port on 0Eh and a third Twinax channel. The "
            "second port reads bit 7 as 0, and the interrupt controller has that layout: Twinax events "
            "07-09h, status 0A-0Ch, display 0Dh, second port 0Eh, keyboard 0Fh (observed: the vectors the "
            "firmware installs; hypothesis: which value bit 7 has on the board)",
            "POST completes; Offline Setup works from the keyboard, and with a saved setup the terminal goes "
            "on line and shows a TN5250 host's screen (observed, straps 4A and 4C; 42 and 48 reach setup); "
            "colour also needs EEPROM byte 30 = AA, which the firmware's own defaults leave at 55 (observed); the "
            "model marks it for a colour unit and power-cycles a new one once (temporary alignment stand-in, "
            "--no-alignment-shim)",
            "44 (3488, type 7) does not start: the firmware's own start-up has no case for type 7 (observed); "
            "signing on, 27x132, second and printer sessions are untested",
            "picture on = 20128 bit 6 (hypothesis); bits 4-3 are per-tube constants (observed), so the 3477's "
            "bit 4 video enable does not apply; colour attribute decoding is the 3477 model's and unverified",
            "--display color selects straps 4C (3487 HC0), paperwhite 48 (3487 HW0); green/amber 42 (3486 "
            "BA0) on model 3486 and 4A (3487 HG0/HA0) on model 3487 (observed decoder, hypothesis for the units)",
            "display test: retrace of 1/140 frame and 20128 bit 3 as sync polarity (hypotheses that satisfy "
            "the test's windows); its required values come from the temporary signature shim",
            "CPU: NEC V20 (D70108L-8) on the logic board (observed), emulated as one: its instruction "
            "timing and edge-case behaviour follow NEC's documentation (src/cpu/i86.cpp); the firmware uses "
            "no 80186-only opcode and no port I/O (observed); NEC's own 0F-prefixed instructions are not "
            "implemented",
            "clock: single 30.000 MHz oscillator (observed); CPU at 7.5 MHz = 30/4 (hypothesis)",
            "system gate array at 20000 and display controller at 20100, 3477-compatible register use "
            "(observed); of the new registers 2000B/2000D/2000F/2001A/2001B (third Twinax channel) and "
            "20144-20146 (second serial port) are modelled, 20017 and the rest of 20140-2014F are plain storage",
            "RAM 384 KiB at 00000-5FFFF (hypothesis; the firmware uses up to about 52000, observed); "
            "optional ROM 60000-7FFFF absent, strap 2000A bit 6 = 1 (observed decoder)",
            "Twinax: three station channels; the third has its station address at 2001A, enable 2001B bit 0, "
            "control 2000B, requests 2000D and mask 2000F (observed in POST's loop-back test), interrupts "
            "on 09h and 0Ch",
            "sessions: two displays, or display + printer (as the 3477; to be confirmed for the 3486)",
            "second keyboard-style serial port at 20144-20146 for a pointing device, interrupt on 0Eh, enable "
            "20007 bit 1 (observed protocol and POST wrap test); modelled with no device attached",
        };
    }


public:
    const ModelInfo& info() const override { return info_; }

    void build(Machine& m, std::vector<LoadedRom>& roms) const override {
        const TerminalOptions& opt = m.options();
        Bus& bus = m.bus();
        const ModelVariant& variant = variant_for(opt);
        const uint8_t straps = opt.straps >= 0 ? uint8_t(opt.straps) : variant.straps;
        if ((straps & 0x0E) == 0x04)
            LOG_WARN("system", "3486: straps %02X select display type 7 (3488), which the 54G0010 firmware "
                               "does not carry through its start-up: expect it to stop in POST "
                               "(docs/bringup-3486.md)", straps);

        parts::battery_backed_ram(m, 0x00000, kRamSize);
        parts::absent_option(m, "option-rom-absent", kOptionRomBase, kOptionRomSize);
        // The alarm tone is not connected on this board's model yet.
        SystemGateArray& sys = parts::system_gate_array(m, info_.clock_hz, straps, kLinkChannels3486, false);
        parts::link_engine(m, sys, info_.clock_hz, kLinkChannels3486, kLinkLayout3486);
        const parts::TwoWireBus i2c = parts::two_wire_bus(m);
        // A colour unit (display type 6) only goes on line with its monitor
        // alignment marked done; part of setting such a unit up, since there
        // is no tube to align [temporary; alignment_shim_3486.hpp]. A new
        // unit is switched off and on once when the firmware has
        // initialised its EEPROM.
        if ((straps & 0x0E) == 0x0C && info_.option_on("alignment-shim", opt.switches)) {
            Machine* machine = &m;
            install_monitor_alignment_shim(i2c.eeprom, [machine] {
                LOG_INFO("machine", "colour unit: power cycle after the alignment was marked done");
                machine->request_power_cycle();
            });
        }
        DisplayController& disp =
            parts::display_controller(m, sys, i2c.bus, info_.clock_hz, kDisplayLayout3486, variant.colour,
                                      info_.option_on("vdo-shim", opt.switches));
        parts::aux_port(m, sys, disp);

        for (LoadedRom& r : roms) {
            if (r.verdict != LoadedRom::Verdict::KnownBad) {
                bus.map_rom(r.spec.file, r.spec.base, std::move(r.data));
                continue;
            }
            // The 3486's 256 KiB chip read as 512 KiB: file blocks 0/1/4/5
            // hold the segments A000/B000/E000/F000 (roms/ROMS.md).
            // 8000/9000/C000/D000 stay unmapped.
            static const struct { uint32_t block, base; const char* name; } kGood[] = {
                {0, 0xA0000, "54g0011.bin (A000)"}, {1, 0xB0000, "54g0011.bin (B000)"},
                {4, 0xE0000, "54g0011.bin (E000)"}, {5, 0xF0000, "54g0011.bin (F000)"},
            };
            for (const auto& g : kGood) {
                const auto first = r.data.begin() + std::ptrdiff_t(g.block) * 0x10000;
                bus.map_rom(g.name, g.base, std::vector<uint8_t>(first, first + 0x10000));
            }
            LOG_WARN("rom", "3486: 54G0011 mapped as segments A000/B000/E000/F000 only; "
                            "8000/9000/C000/D000 are missing and unmapped");
        }
    }

private:
    ModelInfo info_;
};

std::vector<Phosphor> tubes() { return {phosphor_green(), phosphor_amber(), phosphor_paper_white()}; }

// The TN5250 types are the conservative 24x80 ones: what these units
// announce through a real controller has not been established.

class Model3486 : public InfoWindow2Board {
public:
    Model3486()
        : InfoWindow2Board("3486", "IBM 3486 InfoWindow II",
                           {
                               // The firmware's VPD suffix for these straps is BA0 [observed].
                               {"3486-BA", "monochrome", false, false, kDefaultStraps, "IBM-5251-11", tubes()},
                           }) {}
};

class Model3487 : public InfoWindow2Board {
public:
    Model3487()
        : InfoWindow2Board("3487", "IBM 3487 InfoWindow II",
                           {
                               {"3487-HG", "monochrome", false, false, kStraps3487Mono, "IBM-5251-11", tubes()},
                               {"3487-HC", "colour", true, false, kStraps3487Colour, "IBM-3179-2", {}},
                           }) {}
};

} // namespace

const Model& model_3486() {
    static const Model3486 m;
    return m;
}

const Model& model_3487() {
    static const Model3487 m;
    return m;
}

} // namespace em34xx
