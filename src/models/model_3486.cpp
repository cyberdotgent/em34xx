// IBM 3486 InfoWindow II.
//
// Not runnable yet: the only dump of its ROM is incomplete (see
// roms/ROMS.md). The model is prepared from the surviving half so that it is
// close to ready when a complete dump arrives. docs/bringup-3486.md gives the
// evidence for every entry. Summary:
//  - One 512 KiB ROM, 54G0011, mapped at 80000-FFFFF, reset vector at the
//    top [observed: self-referencing pointer tables, reset vector]. The same
//    ROM identifies as 3486, 3487 or 3488 depending on the straps
//    [observed: VPD model table].
//  - The board is a 3477 derivative: system gate array at 20000 and display
//    controller at 20100, with the same interrupt, timer, keyboard, Twinax,
//    printer and I2C registers [observed: same register use]. New
//    registers sit in otherwise unused parts of both pages and are plain
//    storage here.
//  - RAM from 00000; the firmware uses addresses up to about 52000 [observed],
//    and 60000-7FFFF is an optional ROM checked only when a strap says it is
//    fitted [observed]. 384 KiB (00000-5FFFF) is a hypothesis.
//  - 8088-class CPU: no 80186-only opcode in the reachable code, 8086 code
//    generation (repeated single-bit shifts), no port I/O [observed].
// While only the defective dump is available, its four good 64 KiB blocks
// are mapped at the segments they were linked for and the four missing
// segments are left unmapped, so the first access to them stops the machine
// instead of running code from the wrong segment.
#include "models/model.hpp"

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

#include <stdexcept>

namespace tn34xx {

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
    Model3486() {
        info_.name = "3486";
        info_.description = "IBM 3486 InfoWindow II";
        info_.status = ModelStatus::RomIncomplete;
        info_.cpu = CpuLevel::I8088;
        info_.address_bits = 20;
        // 30.000 MHz master oscillator on the logic board (observed); the
        // CPU clock is assumed to be a quarter of it (hypothesis).
        info_.clock_hz = 7500000;
        info_.max_sessions = 2;
        info_.max_printer_sessions = 1;
        info_.roms = {
            {"system ROM (80000-FFFFF, reset vector)", "54g0011.bin", 0x80000, "", 0x80000},
        };
        info_.known_bad_roms = {
            {"54g0011.bin", "f5d968694fdb3b6991b219460eb0ce5eec131797a9c0ea928ec61a6a5b9291ef",
             "read as 27C040 with chip A17 stuck high; segments 8000/9000/C000/D000 missing"},
        };
        info_.notes = {
            "no complete ROM dump yet: POST stops at the ROM checksum (first read of segment 8000); "
            "skipped by hand, it reaches the first far call into segment D000 (observed; see "
            "docs/bringup-3486.md)",
            "20145 bit 7 selects the interrupt layout (observed code): 1 = keyboard on vector 0Eh as on "
            "the 3477, 0 = keyboard on 0Fh, second serial port on 0Eh and a third Twinax channel. The "
            "display page stores 20145, so bit 7 reads 0 and the keyboard handshake runs into an "
            "empty vector 0Eh (observed); which value the 3486 board gives is unknown",
            "one ROM, 54G0011, for 3486/3487/3488: the straps select the model in the VPD (observed)",
            "CPU: NEC V20 (D70108L-8) on the logic board (observed); the firmware uses no 80186-only "
            "opcode and no port I/O (observed), so it is emulated as an 8088 for now; V20-specific "
            "behaviour (undocumented opcodes, timing) is not modelled",
            "clock: single 30.000 MHz oscillator (observed); CPU at 7.5 MHz = 30/4 (hypothesis)",
            "system gate array at 20000 and display controller at 20100, 3477-compatible register use "
            "(observed); new registers 2000D/2000F/20017/2001A/2001B and 20140-2014F are plain storage",
            "RAM 384 KiB at 00000-5FFFF (hypothesis; the firmware uses up to about 52000, observed); "
            "optional ROM 60000-7FFFF absent, strap 2000A bit 6 = 1 (observed decoder)",
            "straps 42: display type 2 = 3486 BA0, mono family (observed decoder, hypothesis for the unit)",
            "ROM word sum over 80000-FFFFF must be 619A (observed POST check); the dump fails it",
            "Twinax: the firmware drives a third station channel (station address 2001A, request 2000D, "
            "mask 2000F) when 20145 bit 7 is 0 (observed code, hypothesis meaning); two channels modelled",
            "sessions: two displays, or display + printer (as the 3477; to be confirmed for the 3486)",
            "second keyboard-style serial port at 20144-20146 (observed protocol; device unknown); storage only",
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

        const uint8_t straps = opt.straps >= 0 ? uint8_t(opt.straps) : kDefaultStraps;
        if (opt.straps < 0 && opt.display != DisplaySubtype::Green)
            LOG_WARN("system", "3486: the display subtype does not change the straps (the 3486 is "
                               "monochrome; colour and other tubes are 3487 strap codes, see "
                               "docs/bringup-3486.md); using straps 42");
        Ibm3477System& sys = m.add_device<Ibm3477System>(info_.clock_hz, straps, std::move(kbd));
        bus.map_mmio("system", Ibm3477System::kBase, Ibm3477System::kSize, &sys);
        bus.set_interrupt_controller(&sys.interrupts());
        m.set_keyboard(kbd_ptr);

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
        disp.set_irq([&irq](bool on) {
            if (on) irq.raise(Ibm3477Interrupts::Source::Display);
            else irq.lower(Ibm3477Interrupts::Source::Display);
        });
        bus.map_mmio("display", Ibm3477Display::kRegBase, 0x100, &disp);
        m.set_display(&disp);

        for (LoadedRom& r : roms) {
            if (r.verdict != LoadedRom::Verdict::KnownBad) {
                bus.map_rom(r.spec.file, r.spec.base, std::move(r.data));
                continue;
            }
            // The A17-stuck dump: file blocks 0/1/4/5 hold the segments
            // A000/B000/E000/F000 (roms/ROMS.md). 8000/9000/C000/D000 stay
            // unmapped.
            static const struct { uint32_t block, base; const char* name; } kGood[] = {
                {0, 0xA0000, "54g0011.bin (A000)"}, {1, 0xB0000, "54g0011.bin (B000)"},
                {4, 0xE0000, "54g0011.bin (E000)"}, {5, 0xF0000, "54g0011.bin (F000)"},
            };
            for (const auto& g : kGood) {
                const auto first = r.data.begin() + std::ptrdiff_t(g.block) * 0x10000;
                bus.map_rom(g.name, g.base, std::vector<uint8_t>(first, first + 0x10000));
            }
            LOG_WARN("rom", "3486: defective dump mapped as segments A000/B000/E000/F000 only; "
                            "8000/9000/C000/D000 are missing and unmapped");
        }
    }

private:
    ModelInfo info_;
};

} // namespace

const Model& model_3486() {
    static const Model3486 m;
    return m;
}

} // namespace tn34xx
