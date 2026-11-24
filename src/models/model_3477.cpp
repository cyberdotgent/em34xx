// SPDX-License-Identifier: GPL-3.0-or-later
// IBM 3477 InfoWindow.
//
// Primary bring-up target. See docs/bringup-3477.md for the memory and
// register map and the evidence for each entry. Summary:
//  - Two byte-wide 128 KiB ROMs, 95F4171 (E0000-FFFFF, reset vector) and
//    95F4172 (C0000-DFFFF), each a linear image.
//  - 128 KiB of RAM at 00000-1FFFF, including the display window and the
//    character-generator RAM (18000-1FFFF) [observed: tested and cleared
//    by the firmware].
//  - System gate array at 20000 (interrupts, timer, straps, keyboard,
//    Twinax, printer), display controller at 20100 [observed].
//  - Optional RAM at 40000-7FFFF and an optional feature ROM at A0000-BFFFF,
//    both absent in this configuration. The firmware detects them by
//    probing, so the ranges read as open bus (FF) without stopping the
//    machine [observed: probes at boot].
// Hypotheses (to be confirmed on hardware):
//  - CPU is 8088-class (byte-wide ROMs; no 80186-only opcode is executed).
//  - Clock frequency unknown; 8 MHz is a pacing placeholder only.
#include "models/model.hpp"

#include "models/board_3477.hpp"
#include "models/parts.hpp"
#include "machine/machine.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <stdexcept>

namespace em34xx {

namespace {

class Model3477 : public Model {
public:
    Model3477() {
        info_.name = "3477";
        info_.description = "IBM 3477 InfoWindow";
        info_.status = ModelStatus::Bringup;
        info_.cpu = CpuLevel::I8088;
        info_.address_bits = 20;
        // 59.8185 MHz crystal (documented, MAME board notes); CPU at
        // crystal/8 (hypothesis).
        info_.clock_hz = 7477312;
        info_.line = LineKind::Twinax;
        info_.max_sessions = 2;
        info_.max_printer_sessions = 1;   // display + printer [doc PAI 14.0 via 3197; obs]
        const std::vector<Phosphor> tubes = {phosphor_green(), phosphor_amber(), phosphor_paper_white()};
        info_.variants = {
            // One monochrome unit whatever tube is fitted; it announces the
            // green type. (The firmware's own VPD suffix for these straps
            // is FE0 [observed]; FG is the green 3477's model and TN5250
            // name [documented].)
            {"3477-FG", "monochrome", false, false, kStraps3477Mono, "IBM-3477-FG", tubes},
            // Stand-in until the colour 3477's own ROM is emulated: this
            // ROM cannot identify as colour (see the notes), so the unit
            // runs as the monochrome one and only the host and the picture
            // treat it as colour.
            {"3477-FC", "colour (stand-in)", true, true, kStraps3477Mono, "IBM-3477-FC", {}},
        };
        info_.options = {
            {"vdo-shim", "TEMPORARY: stand in for the unmodelled video signature of the power-on display test "
                         "(off: POST stops at VDO)", true},
        };
        info_.roms = {
            {"system ROM (C0000-DFFFF)", "95f4172.bin", 0x20000,
             "290a58cd5db6543957bdcb2c9e92efefd62a54e37bcd239d41d9bd9cdd6be1d0", 0xC0000},
            {"system ROM (E0000-FFFFF, reset vector)", "95f4171.bin", 0x20000,
             "3f7fb58aec7505315ff94206c8dc05102e319944bd0bde55092274777fb39fde", 0xE0000},
        };
        info_.notes = {
            "two twinax sessions: two displays, or display + printer (documented, PAI 1989 3197/3477)",
            "CPU: AMD N8088-2 on the logic board (documented, MAME ibm3477 board notes)",
            "RAM 128 KiB at 00000-1FFFF (observed); optional RAM 40000-7FFFF and feature ROM "
            "A0000-BFFFF absent (open bus)",
            "clock: 59.8185 MHz crystal (documented); CPU at crystal/8 = 7.48 MHz (hypothesis); "
            "dot clock = crystal with 16-dot cells gives 35.95 kHz lines (hypothesis)",
            "system gate array at 20000: interrupt request/mask registers and fixed vectors "
            "(observed use, bit map hypothesis), 50 Hz timer (hypothesis)",
            "display type straps at 2000A: 04/0C/0E = types 3/4/5, reported as model FD0/FE0/FW0 "
            "with model ID 24 (observed); green/amber -> 0C, paper-white -> 0E (hypothesis)",
            "colour: the firmware has a complete colour path (display type 6 = FC0, model ID 50, "
            "4 ECB planes) but nothing in this ROM selects type 6 (observed); the 3477-FC variant is a stand-in that renders the "
            "attributes in colour on a unit that identifies as FE0, model ID 24",
            "setup configuration in battery-backed RAM: POST never tests 01A0-0A9F and only "
            "validates the checksummed setup block there; an hour meter and a power-on counter "
            "live in the same range (observed); battery backing of the whole 128 KiB is a hypothesis",
            "I2C EEPROM at A0 (24C02-style, size hypothesis): monitor alignment values, read and "
            "written only in a service mode; not the setup configuration (observed)",
            "Twinax engine: two station channels, programs and descriptors in RAM; poll, activate and "
            "queue handling and the loopback test path follow docs/bringup-3477.md (hypothesis)",
            "display controller at 20100: character window from RAM, glyphs from character-generator "
            "RAM; POST test VDO compares a 4-bit video signature (2012F bits 7-4) whose function is "
            "unknown; a TEMPORARY shim returns measured results for the test screens, keyed on the "
            "selector and a fingerprint of the displayed frame (--no-vdo-shim turns it off; "
            "see docs/bringup-3477.md)",
        };
    }

    const ModelInfo& info() const override { return info_; }

    void build(Machine& m, std::vector<LoadedRom>& roms) const override {
        const TerminalOptions& opt = m.options();
        const ModelVariant& variant = variant_for(opt);
        const uint8_t straps = opt.straps >= 0 ? uint8_t(opt.straps) : variant.straps;
        if (variant.stub)
            LOG_WARN("system", "3477: this ROM has no colour display type; the terminal identifies as a "
                               "monochrome 3477 FE0 (model ID 24h) and only the rendering is in colour");

        // 128 KiB of RAM; the setup configuration and the counters live in
        // it and survive power-off [hypothesis, strong: see the notes].
        parts::battery_backed_ram(m, 0x00000, 0x20000);
        parts::absent_option(m, "xram-absent", 0x40000, 0x40000);
        parts::absent_option(m, "feature-rom-absent", 0xA0000, 0x20000);
        SystemGateArray& sys = parts::system_gate_array(m, info_.clock_hz, straps, kLinkChannels3477, true);
        parts::link_engine(m, sys, info_.clock_hz, kLinkChannels3477, kLinkLayout3477);
        const parts::TwoWireBus i2c = parts::two_wire_bus(m);
        parts::display_controller(m, sys, i2c.bus, info_.clock_hz, kDisplayLayout3477, variant.colour,
                                  info_.option_on("vdo-shim", opt.switches));
        for (LoadedRom& r : roms) m.bus().map_rom(r.spec.file, r.spec.base, std::move(r.data));
    }

private:
    ModelInfo info_;
};

} // namespace

const Model& model_3477() {
    static const Model3477 m;
    return m;
}

} // namespace em34xx
