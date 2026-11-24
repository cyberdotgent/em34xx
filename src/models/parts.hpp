// SPDX-License-Identifier: GPL-3.0-or-later
// The parts a terminal of this family is built from. A model's build() is a
// list of these calls with its board's descriptions (board_*.hpp): each
// creates the hardware (src/hw), maps it, connects it to the parts it
// depends on and registers it with the machine. Errors (a file that cannot
// be opened, a bad keyboard option) are thrown as std::runtime_error, which
// Application::make_machine reports.
#pragma once

#include "hw/aux_port.hpp"
#include "hw/battery_ram.hpp"
#include "hw/display_controller.hpp"
#include "hw/i2c.hpp"
#include "hw/i2c_eeprom.hpp"
#include "hw/system_gate_array.hpp"
#include "hw/twinax_engine.hpp"

#include <cstdint>

namespace em34xx {

class Machine;

namespace parts {

// RAM at `base`, kept across power-off in the machine's battery-backed RAM
// image (TerminalOptions::nvram_path).
BatteryBackedRam& battery_backed_ram(Machine& m, uint32_t base, uint32_t size);

// An address range where optional hardware is absent: reads float high,
// writes go nowhere. Mapped explicitly because the firmware probes it on
// every boot, and probing an absent option is not an unexpected access.
void absent_option(Machine& m, const char* name, uint32_t base, uint32_t size);

// The system gate array with the keyboard the options describe plugged in
// (type, layout, ID, keymap): interrupt controller, timer, straps, keyboard
// port. `link_channels`: 2 or 3 (SystemGateArray). `alarm`: its tone and
// speaker gate drive the machine's sound.
SystemGateArray& system_gate_array(Machine& m, uint64_t clock_hz, uint8_t straps, unsigned link_channels,
                                   bool alarm);

// The line (Twinax) engine with `channels` station channels, its events
// latched in the gate array's request registers (channels 0 and 1 in the
// two bytes of the first, a third in the second) and its registers in the
// gate array's page; the machine's line host is on the other side.
TwinaxEngine& link_engine(Machine& m, SystemGateArray& sys, uint64_t clock_hz, unsigned channels,
                          const TwinaxEngineLayout& layout);

// The two-wire bus with the monitor controller (8C, write-only) and the
// setup EEPROM (A0), kept in TerminalOptions::eeprom_path.
struct TwoWireBus {
    I2cBus& bus;
    I2cEeprom& eeprom;
};
TwoWireBus two_wire_bus(Machine& m);

// The display controller, with its interrupt in the gate array and the
// two-wire bus on its bit-bang lines; the machine's display.
DisplayController& display_controller(Machine& m, SystemGateArray& sys, I2cBus& i2c, uint64_t clock_hz,
                                      const DisplayLayout& layout, bool colour, bool vdo_shim);

// The second serial port (pointing device), whose registers sit in the
// display controller's page.
AuxPort& aux_port(Machine& m, SystemGateArray& sys, DisplayController& display);

} // namespace parts
} // namespace em34xx
