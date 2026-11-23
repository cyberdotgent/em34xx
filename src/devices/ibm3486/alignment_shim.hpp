// SPDX-License-Identifier: GPL-3.0-or-later
// 3486 family, colour units: the monitor alignment the unit never got.
//
// Observed (docs/bringup-3486.md, "EEPROM and monitor alignment"): at
// power-on the firmware of a colour unit reads EEPROM byte 00; when that is
// not AA it writes its default alignment values to 00-37, byte 00 (AA)
// last. POST then requires AA at 00 and at 30, but those defaults leave 30
// at another value, and the unit shows an alignment pattern instead of going
// on line. Hypothesis: 30 = AA means "monitor aligned", set by the
// alignment procedure a factory or service technician completes on the
// real tube; how that procedure is driven is not known, and there is no
// tube to align.
//
// The firmware reads the EEPROM once per power-on: in the start in which it
// wrote its defaults it shows the pattern whatever byte 30 becomes
// afterwards, and with 30 = AA it goes on line from the next power-on
// (observed: the same reads with the same values in both starts). Marking
// a blank EEPROM before the first power-on does not help either: with 00 =
// AA and the other bytes blank the firmware writes its defaults again
// (observed).
//
// TEMPORARY stand-in for that procedure, part of the colour unit's setup:
// whenever the EEPROM holds the firmware's "initialised" mark (00 = AA),
// byte 30 is made AA as well, and when that happens while the firmware
// runs (a new unit's first power-on) the unit is switched off and on once,
// as after an alignment. The alignment values themselves stay the
// firmware's own; none are stored in the emulator. Switch:
// TerminalOptions::alignment_shim / --no-alignment-shim / [emulator]
// alignment_shim.
#pragma once

#include "devices/ibm3477/i2c.hpp"
#include "util/log.hpp"

#include <functional>
#include <utility>

namespace em34xx {

constexpr size_t kAlignmentInitialisedByte = 0x00;
constexpr size_t kAlignmentDoneByte = 0x30;
constexpr uint8_t kAlignmentMark = 0xAA;

// Marks the alignment done when the EEPROM is initialised; returns whether
// it changed byte 30.
inline bool mark_monitor_aligned(I2cEeprom& eeprom) {
    if (eeprom.byte(kAlignmentInitialisedByte) != kAlignmentMark || eeprom.byte(kAlignmentDoneByte) == kAlignmentMark)
        return false;
    eeprom.set_byte(kAlignmentDoneByte, kAlignmentMark);
    LOG_INFO("eeprom", "colour unit: monitor alignment marked done (EEPROM byte 30 = AA; temporary stand-in "
                       "for the alignment procedure)");
    return true;
}

// An image the firmware initialised earlier is marked now, before the
// power-on that reads it; a blank one when the firmware writes its
// "initialised" mark, and `power_cycle` is then called (the firmware only
// reads the byte at power-on). The EEPROM must outlive the hook (both
// belong to the machine).
inline void install_monitor_alignment_shim(I2cEeprom& eeprom, std::function<void()> power_cycle) {
    mark_monitor_aligned(eeprom);
    I2cEeprom* e = &eeprom;
    eeprom.set_write_observer([e, power_cycle = std::move(power_cycle)](size_t address, uint8_t value) {
        if (address == kAlignmentInitialisedByte && value == kAlignmentMark && mark_monitor_aligned(*e) && power_cycle)
            power_cycle();
    });
}

} // namespace em34xx
