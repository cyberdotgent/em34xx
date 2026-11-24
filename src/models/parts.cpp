// SPDX-License-Identifier: GPL-3.0-or-later
#include "models/parts.hpp"

#include "hw/keyboard_interface.hpp"
#include "input/host_keymap.hpp"
#include "input/keyboard_ids.hpp"
#include "machine/machine.hpp"

#include <memory>
#include <stdexcept>
#include <string>

namespace em34xx {
namespace parts {

namespace {

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
    // Write-only device at 8C [hypothesis on the 3477; assumed the same on
    // the 3486 family].
    I2cLogDevice monitor_ctl_{"monitor controller"};
};

} // namespace

BatteryBackedRam& battery_backed_ram(Machine& m, uint32_t base, uint32_t size) {
    m.bus().map_ram("ram", base, size);
    BatteryBackedRam& nvram = m.add_device<BatteryBackedRam>("nvram", m.bus(), base, size);
    std::string error;
    if (!nvram.open(m.options().nvram_path, error)) throw std::runtime_error(error);
    return nvram;
}

void absent_option(Machine& m, const char* name, uint32_t base, uint32_t size) {
    AbsentDevice& d = m.add_device<AbsentDevice>(name);
    m.bus().map_mmio(name, base, size, &d);
}

SystemGateArray& system_gate_array(Machine& m, uint64_t clock_hz, uint8_t straps, unsigned link_channels,
                                   bool alarm) {
    const TerminalOptions& opt = m.options();
    uint8_t id1 = 0xBF, id2 = 0xBF;
    std::string error;
    if (!resolve_keyboard_id(opt, id1, id2, error)) throw std::runtime_error(error);
    KeyboardType type = KeyboardType::Terminal122;
    parse_keyboard_type(opt.keyboard_type, type);
    auto kbd = std::make_unique<KeyboardInterface>(clock_hz, type, id1, id2);
    kbd->keyboard().set_shift_fkeys(opt.shift_fkeys);
    auto keymap = std::make_shared<Keymap>();
    if (!load_keymap(opt.keymap, opt.shift_fkeys, *keymap, error)) throw std::runtime_error(error);
    kbd->keyboard().set_keymap(std::move(keymap));
    KeyboardInterface* kbd_ptr = kbd.get();

    SystemGateArray& sys = m.add_device<SystemGateArray>(clock_hz, straps, link_channels, std::move(kbd));
    m.bus().map_mmio("system", SystemGateArray::kBase, SystemGateArray::kSize, &sys);
    if (alarm) sys.set_audio(&m.audio());
    m.bus().set_interrupt_controller(&sys.interrupts());
    m.set_keyboard(kbd_ptr);
    return sys;
}

TwinaxEngine& link_engine(Machine& m, SystemGateArray& sys, uint64_t clock_hz, unsigned channels,
                          const TwinaxEngineLayout& layout) {
    TwinaxEngine& link = m.add_device<TwinaxEngine>(m.bus(), channels, clock_hz, layout);
    GateArrayInterrupts& irq = sys.interrupts();
    // Channels 0 and 1 report in the low and high byte of the request
    // register, a third channel in the second register (2000D/2000F).
    link.set_raise([&irq](unsigned ch, uint8_t bits) {
        if (ch == 2) irq.request2(bits);
        else irq.request(uint16_t(bits << (8 * ch)));
    });
    link.set_pending([&irq](unsigned ch) {
        return uint8_t((ch == 2 ? irq.requests2() : uint8_t(irq.requests() >> (8 * ch))) & 0x7F);
    });
    Machine* machine = &m;
    link.set_host([machine]() { return machine->twinax_host(); });
    TwinaxRegisters& regs = m.add_device<TwinaxRegisters>(link);
    sys.set_twinax_port(&regs);
    return link;
}

TwoWireBus two_wire_bus(Machine& m) {
    I2cBusDevice& i2c = m.add_device<I2cBusDevice>();
    I2cEeprom& eeprom = m.add_device<I2cEeprom>("eeprom");
    std::string error;
    if (!eeprom.open(m.options().eeprom_path, error)) throw std::runtime_error(error);
    eeprom.attach(i2c.bus(), 0xA0);
    m.set_nvram(&eeprom);
    return TwoWireBus{i2c.bus(), eeprom};
}

DisplayController& display_controller(Machine& m, SystemGateArray& sys, I2cBus& i2c, uint64_t clock_hz,
                                      const DisplayLayout& layout, bool colour, bool vdo_shim) {
    DisplayController& disp = m.add_device<DisplayController>(m.bus(), layout, colour, vdo_shim);
    disp.set_clock_hz(clock_hz);
    disp.set_i2c(&i2c);
    GateArrayInterrupts& irq = sys.interrupts();
    disp.set_irq([&irq](bool on) {
        if (on) irq.raise(GateArrayInterrupts::Source::Display);
        else irq.lower(GateArrayInterrupts::Source::Display);
    });
    m.bus().map_mmio("display", DisplayController::kRegBase, 0x100, &disp);
    m.set_display(&disp);
    return disp;
}

AuxPort& aux_port(Machine& m, SystemGateArray& sys, DisplayController& display) {
    AuxPort& aux = m.add_device<AuxPort>();
    aux.connect(&sys.interrupts());
    display.set_aux_port(&aux);
    return aux;
}

} // namespace parts
} // namespace em34xx
