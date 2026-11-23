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
//  - 8088-class CPU: no 80186-only opcode in the reachable code, 8086 code
//    generation (repeated single-bit shifts), no port I/O [observed].
// The 3486's own chip, 54G0011, holds only half of its firmware. Loaded with
// --allow-bad-rom, its four 64 KiB blocks are mapped at the segments they
// were linked for and the four missing segments are left unmapped, so the
// first access to them stops the machine instead of running code from the
// wrong segment.
#include "models/model.hpp"

#include "devices/battery_ram.hpp"
#include "devices/ibm3486/alignment_shim.hpp"
#include "devices/ibm3486/aux_port.hpp"
#include "devices/i2c.hpp"
#include "devices/ibm3477/display.hpp"
#include "devices/ibm3477/i2c.hpp"
#include "devices/ibm3477/keyboard_if.hpp"
#include "devices/ibm3477/system.hpp"
#include "devices/ibm3477/twinax_engine.hpp"
#include "input/host_keymap.hpp"
#include "input/keyboard_ids.hpp"
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

// Straps for a display subtype [observed decoder; which unit has which tube
// is a hypothesis]: colour is the 3487 HC0 (type 6) and paper-white the 3487
// HW0 (type 5). Green and amber are the 3486 (type 2) on the 3486 model and
// the 3487 HG0/HA0 (types 3/4, straps 4A) on the 3487 model.
uint8_t default_straps(bool is_3487, DisplaySubtype d) {
    switch (d) {
    case DisplaySubtype::Color: return 0x4C;
    case DisplaySubtype::PaperWhite: return 0x48;
    default: return is_3487 ? 0x4A : kDefaultStraps;
    }
}

// Vertical retrace as a fraction of the frame. The display test requires the
// retrace flag (2012F bit 3) to drop within 3 to 9 status reads of its start
// and to come back one field later [observed windows]; the 3477's 1/90 lasts
// 10 reads here. 1/140 (about 120 us) is in the middle [hypothesis: the real
// length is not known].
constexpr unsigned kRetraceDivisor = 140;

// An address range where optional hardware is absent: reads float high,
// writes go nowhere.
class AbsentDevice : public Device {
public:
    using Device::Device;
    uint8_t mmio_read8(uint32_t) override { return 0xFF; }
    void mmio_write8(uint32_t, uint8_t) override {}
    uint8_t peek8(uint32_t) override { return 0xFF; }
    std::string describe() const override { return "  absent option: reads FF, ignores writes\n"; }
};

// Owner of the two-wire bus (bit-banged through 2012F, as on the 3477).
class I2cBusDevice : public Device {
public:
    I2cBusDevice() : Device("i2c") { bus_.attach(0x8C >> 1, &monitor_ctl_); }
    I2cBus& bus() { return bus_; }
    void reset() override { bus_.reset(); }
    std::string describe() const override { return bus_.describe(); }

private:
    I2cBus bus_;
    I2cLogDevice monitor_ctl_{"monitor controller"};   // 3477 device at 8C [hypothesis for the 3486]
};

class Model3486 : public Model {
public:
    // The 3486 and the 3487 are one board and one firmware; they differ in
    // the tube and therefore in the default straps.
    explicit Model3486(bool is_3487) : is_3487_(is_3487) {
        info_.name = is_3487 ? "3487" : "3486";
        info_.description = is_3487 ? "IBM 3487 InfoWindow II" : "IBM 3486 InfoWindow II";
        info_.status = ModelStatus::Bringup;
        info_.cpu = CpuLevel::I8088;
        info_.address_bits = 20;
        // 30.000 MHz master oscillator on the logic board (observed); the
        // CPU clock is assumed to be a quarter of it (hypothesis).
        info_.clock_hz = 7500000;
        info_.max_sessions = 2;
        info_.max_printer_sessions = 1;
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
            "CPU: NEC V20 (D70108L-8) on the logic board (observed); the firmware uses no 80186-only "
            "opcode and no port I/O (observed), so it is emulated as an 8088 for now; V20-specific "
            "behaviour (undocumented opcodes, timing) is not modelled",
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

    const ModelInfo& info() const override { return info_; }

    void build(Machine& m, std::vector<LoadedRom>& roms) const override {
        const TerminalOptions& opt = m.options();
        Bus& bus = m.bus();

        bus.map_ram("ram", 0x00000, kRamSize);
        BatteryBackedRam& nvram = m.add_device<BatteryBackedRam>("nvram", bus, 0x00000u, kRamSize);
        AbsentDevice& option = m.add_device<AbsentDevice>("option-rom-absent");
        bus.map_mmio("option-rom-absent", kOptionRomBase, kOptionRomSize, &option);

        uint8_t id1 = 0xBF, id2 = 0xBF;
        std::string error;
        if (!nvram.open(opt.nvram_path, error)) throw std::runtime_error(error);
        if (!resolve_keyboard_id(opt, id1, id2, error)) throw std::runtime_error(error);
        KeyboardType type = KeyboardType::Terminal122;
        parse_keyboard_type(opt.keyboard_type, type);
        auto kbd = std::make_unique<Ibm3477KeyboardInterface>(info_.clock_hz, type, id1, id2);
        kbd->keyboard().set_shift_fkeys(opt.shift_fkeys);
        auto keymap = std::make_shared<Keymap>();
        if (!load_keymap(opt.keymap, opt.shift_fkeys, *keymap, error)) throw std::runtime_error(error);
        kbd->keyboard().set_keymap(std::move(keymap));
        Ibm3477KeyboardInterface* kbd_ptr = kbd.get();

        const uint8_t straps = opt.straps >= 0 ? uint8_t(opt.straps) : default_straps(is_3487_, opt.display);
        if ((straps & 0x0E) == 0x04)
            LOG_WARN("system", "3486: straps %02X select display type 7 (3488), which the 54G0010 firmware "
                               "does not carry through its start-up: expect it to stop in POST "
                               "(docs/bringup-3486.md)", straps);
        Ibm3477System& sys = m.add_device<Ibm3477System>(info_.clock_hz, straps, std::move(kbd));
        bus.map_mmio("system", Ibm3477System::kBase, Ibm3477System::kSize, &sys);
        sys.interrupts().set_three_channel_layout();
        bus.set_interrupt_controller(&sys.interrupts());
        m.set_keyboard(kbd_ptr);

        // Three station channels; the third reports through 2000D/2000F.
        sys.set_third_channel(true);
        TwinaxEngine& link = m.add_device<TwinaxEngine>(bus, 3u, info_.clock_hz);
        Ibm3477Interrupts& irq = sys.interrupts();
        link.set_raise([&irq](unsigned ch, uint8_t bits) {
            if (ch == 2) irq.request2(bits);
            else irq.request(uint16_t(bits << (8 * ch)));
        });
        link.set_pending([&irq](unsigned ch) {
            return uint8_t((ch == 2 ? irq.requests2() : uint8_t(irq.requests() >> (8 * ch))) & 0x7F);
        });
        link.set_host([&m]() { return m.twinax_host(); });
        Ibm3477TwinaxRegisters& link_regs = m.add_device<Ibm3477TwinaxRegisters>(link);
        sys.set_twinax_port(&link_regs);

        I2cBusDevice& i2c = m.add_device<I2cBusDevice>();
        I2cEeprom& eeprom = m.add_device<I2cEeprom>("eeprom");
        if (!eeprom.open(opt.eeprom_path, error)) throw std::runtime_error(error);
        eeprom.attach(i2c.bus(), 0xA0);
        m.set_nvram(&eeprom);
        // A colour unit (display type 6) only goes on line with its monitor
        // alignment marked done; part of setting such a unit up, since there
        // is no tube to align [temporary; alignment_shim.hpp]. A new unit
        // is switched off and on once when the firmware has initialised its
        // EEPROM.
        if ((straps & 0x0E) == 0x0C && opt.alignment_shim) {
            Machine* machine = &m;
            install_monitor_alignment_shim(eeprom, [machine] {
                LOG_INFO("machine", "colour unit: power cycle after the alignment was marked done");
                machine->request_power_cycle();
            });
        }

        Ibm3477Display& disp = m.add_device<Ibm3477Display>(bus, opt);
        disp.set_clock_hz(info_.clock_hz);
        disp.set_retrace_divisor(kRetraceDivisor);
        disp.set_sync_polarity_bit(true);
        disp.set_signature_selector_shift(4);
        disp.set_video_enable_bit6(true);
        disp.set_i2c(&i2c.bus());
        disp.set_irq([&irq](bool on) {
            if (on) irq.raise(Ibm3477Interrupts::Source::Display);
            else irq.lower(Ibm3477Interrupts::Source::Display);
        });
        Ibm3486AuxPort& aux = m.add_device<Ibm3486AuxPort>(info_.clock_hz);
        aux.connect(&irq);
        if (opt.pointing_device == "mouse") {
            aux.attach_mouse(true);
            m.set_pointer(&aux);
        } else if (opt.pointing_device != "none" && !opt.pointing_device.empty()) {
            throw std::runtime_error("unknown pointing device '" + opt.pointing_device + "' (none, mouse)");
        }
        disp.set_aux_port(&aux);
        bus.map_mmio("display", Ibm3477Display::kRegBase, 0x100, &disp);
        m.set_display(&disp);

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

    bool display_change_needs_power_cycle(const TerminalOptions& current, DisplaySubtype to) const override {
        if (Model::display_change_needs_power_cycle(current, to)) return true;
        // The display-type straps (2000A) follow the subtype unless they are
        // given explicitly, and the firmware reads them at power-on.
        return current.straps < 0 && default_straps(is_3487_, current.display) != default_straps(is_3487_, to);
    }

private:
    bool is_3487_;
    ModelInfo info_;
};

} // namespace

const Model& model_3486() {
    static const Model3486 m(false);
    return m;
}

const Model& model_3487() {
    static const Model3486 m(true);
    return m;
}

} // namespace em34xx
