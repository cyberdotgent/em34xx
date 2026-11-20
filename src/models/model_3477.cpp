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

#include "config/config.hpp"
#include "devices/battery_ram.hpp"
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
#include "util/strings.hpp"

#include <stdexcept>

namespace tn34xx {

namespace {

// An address range where optional hardware is absent: reads float high,
// writes go nowhere. Mapped explicitly because the firmware probes it on
// every boot, and probing an absent option is not an unexpected access.
class AbsentDevice : public Device {
public:
    using Device::Device;
    uint8_t mmio_read8(uint32_t) override { return 0xFF; }
    void mmio_write8(uint32_t, uint8_t) override {}
    uint8_t peek8(uint32_t) override { return 0xFF; }
    std::string describe() const override { return "  absent option: reads FF, ignores writes\n"; }
};

// Owner of the two-wire bus (bit-banged through 2012F).
class I2cBusDevice : public Device {
public:
    I2cBusDevice() : Device("i2c") { bus_.attach(0x8C >> 1, &monitor_ctl_); }
    I2cBus& bus() { return bus_; }
    void reset() override { bus_.reset(); }
    std::string describe() const override { return bus_.describe(); }

private:
    I2cBus bus_;
    I2cLogDevice monitor_ctl_{"monitor controller"};   // write-only device at 8C [hypothesis]
};

class Model3477 : public Model {
public:
    Model3477() {
        info_.name = "3477";
        info_.description = "IBM 3477 InfoWindow";
        info_.status = ModelStatus::Bringup;
        info_.cpu = CpuLevel::I8088;
        info_.address_bits = 20;
        info_.clock_hz = 8000000;
        info_.max_sessions = 2;
        info_.max_printer_sessions = 1;   // display + printer [doc PAI 14.0 via 3197; obs]
        info_.roms = {
            {"system ROM (C0000-DFFFF)", "95f4172.bin", 0x20000,
             "290a58cd5db6543957bdcb2c9e92efefd62a54e37bcd239d41d9bd9cdd6be1d0", 0xC0000},
            {"system ROM (E0000-FFFFF, reset vector)", "95f4171.bin", 0x20000,
             "3f7fb58aec7505315ff94206c8dc05102e319944bd0bde55092274777fb39fde", 0xE0000},
        };
        info_.notes = {
            "two twinax sessions: two displays, or display + printer (documented, PAI 1989 3197/3477)",
            "CPU 8088 is inferred from byte-wide linear ROM images (hypothesis)",
            "RAM 128 KiB at 00000-1FFFF (observed); optional RAM 40000-7FFFF and feature ROM "
            "A0000-BFFFF absent (open bus)",
            "clock frequency unknown; 8 MHz used for pacing only",
            "system gate array at 20000: interrupt request/mask registers and fixed vectors "
            "(observed use, bit map hypothesis), 50 Hz timer (hypothesis)",
            "display type straps at 2000A: 04/0C/0E = types 3/4/5, reported as model FD0/FE0/FW0 "
            "with model ID 24 (observed); green/amber -> 0C, paper-white -> 0E (hypothesis)",
            "colour: the firmware has a complete colour path (display type 6 = FC0, model ID 50, "
            "4 ECB planes) but nothing in this ROM selects type 6 (observed); --color renders the "
            "attributes in colour on a unit that identifies as FE0, model ID 24",
            "setup configuration in battery-backed RAM: POST never tests 01A0-0A9F and only "
            "validates the checksummed setup block there; an hour meter and a power-on counter "
            "live in the same range (observed); battery backing of the whole 128 KiB is a hypothesis",
            "I2C EEPROM at A0 (24C02-style, size hypothesis): monitor alignment values, read and "
            "written only in a service mode; not the setup configuration (observed)",
            "Twinax engine: two station channels, programs and descriptors in RAM; poll, activate and "
            "queue handling and the loopback test path follow docs/bringup-3477.md (hypothesis)",
            "display controller at 20100: character window from RAM, glyphs from character-generator "
            "RAM; POST test VDO compares a 4-bit video signature (2012F bits 7-4) that is not "
            "modelled, so POST stops there (see docs/bringup-3477.md)",
        };
    }

    const ModelInfo& info() const override { return info_; }

    void build(Machine& m, std::vector<LoadedRom>& roms) const override {
        const TerminalOptions& opt = m.options();
        Bus& bus = m.bus();

        bus.map_ram("ram", 0x00000, 0x20000);
        // The setup configuration and the counters live in RAM that keeps
        // its contents across power-off [hypothesis, strong: see the notes].
        BatteryBackedRam& nvram = m.add_device<BatteryBackedRam>("nvram", bus, 0x00000u, 0x20000u);
        AbsentDevice& xram = m.add_device<AbsentDevice>("xram-absent");
        bus.map_mmio("xram-absent", 0x40000, 0x40000, &xram);
        AbsentDevice& feature = m.add_device<AbsentDevice>("feature-rom-absent");
        bus.map_mmio("feature-rom-absent", 0xA0000, 0x20000, &feature);

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

        const uint8_t straps = opt.straps >= 0 ? uint8_t(opt.straps) : ibm3477_default_straps(opt.display);
        if (opt.display == DisplaySubtype::Color)
            LOG_WARN("system", "3477: this ROM has no colour display type; the terminal identifies as a "
                               "monochrome 3477 FE0 (model ID 24h) and only the rendering is in colour");
        Ibm3477System& sys = m.add_device<Ibm3477System>(info_.clock_hz, straps, std::move(kbd));
        bus.map_mmio("system", Ibm3477System::kBase, Ibm3477System::kSize, &sys);
        bus.set_interrupt_controller(&sys.interrupts());
        m.set_keyboard(kbd_ptr);

        // Twinax link engine: two station channels, structures in low RAM,
        // events latched in the gate array's request register (channel 0 in
        // the low byte, channel 1 in the high byte).
        TwinaxEngine& link = m.add_device<TwinaxEngine>(bus, 2u, info_.clock_hz);
        Ibm3477Interrupts& irq = sys.interrupts();
        link.set_raise([&irq](unsigned ch, uint8_t bits) { irq.request(uint16_t(bits << (8 * ch))); });
        link.set_pending([&irq](unsigned ch) { return uint8_t((irq.requests() >> (8 * ch)) & 0x7F); });
        link.set_host([&m]() { return m.twinax_host(); });
        Ibm3477TwinaxRegisters& link_regs = m.add_device<Ibm3477TwinaxRegisters>(link);
        sys.set_twinax_port(&link_regs);

        I2cBusDevice& i2c = m.add_device<I2cBusDevice>();
        I2cEeprom& eeprom = m.add_device<I2cEeprom>("eeprom");
        if (!eeprom.open(opt.eeprom_path, error)) throw std::runtime_error(error);
        eeprom.attach(i2c.bus(), 0xA0);
        m.set_nvram(&eeprom);

        Ibm3477Display& disp = m.add_device<Ibm3477Display>(bus, opt);
        disp.set_clock_hz(info_.clock_hz);
        disp.set_i2c(&i2c.bus());
        Ibm3477Interrupts& ic = sys.interrupts();
        disp.set_irq([&ic](bool on) {
            if (on) ic.raise(Ibm3477Interrupts::Source::Display);
            else ic.lower(Ibm3477Interrupts::Source::Display);
        });
        bus.map_mmio("display", Ibm3477Display::kRegBase, 0x100, &disp);
        m.set_display(&disp);

        for (LoadedRom& r : roms) bus.map_rom(r.spec.file, r.spec.base, std::move(r.data));
    }

private:
    ModelInfo info_;
};

} // namespace

const Model& model_3477() {
    static const Model3477 m;
    return m;
}

} // namespace tn34xx
