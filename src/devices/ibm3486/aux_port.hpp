// SPDX-License-Identifier: GPL-3.0-or-later
// Second keyboard-style serial port of the 3486 family (display page
// registers 20144-20146, interrupt enable 20007 bit 1). It takes a pointing
// device: a PS/2 mouse or a light pen [observed by a unit's owner]. See
// docs/bringup-3486.md.
//
// Same shift-register design as the keyboard interface (keyboard_if.hpp):
//   20144 W  byte to send; R  last byte received
//   20145 W  line control: bit 0 = line held, bit 1 = clock. While the line
//            is held, each 0->1 transition of bit 1 clocks the register by
//            hand; after 11 clocks the byte written to 20144 has looped back
//            into the receive side and the port interrupts [observed: POST's
//            wrap test]
//   20145 R  bit 0 = received odd-parity bit, bit 6 = byte received
//            [observed use]; bit 7 selects the board's interrupt layout and
//            reads 0 here [hypothesis]
//   20146 W  clear the receive status and the interrupt
// No device is modelled yet: nothing answers a byte sent with the line
// released, as with an empty jack.
#pragma once

#include "bus/device.hpp"

#include <cstdint>
#include <string>

namespace em34xx {

class Ibm3477Interrupts;

class Ibm3486AuxPort : public Device {
public:
    Ibm3486AuxPort() : Device("aux-port") {}

    void connect(Ibm3477Interrupts* ic) { ic_ = ic; }

    // Register access; reg 0 = 20144, 1 = 20145, 2 = 20146.
    uint8_t read(unsigned reg) const;
    void write(unsigned reg, uint8_t value);

    void reset() override;
    std::string describe() const override;

private:
    Ibm3477Interrupts* ic_ = nullptr;
    uint8_t ctl_ = 0x03;
    uint8_t tx_ = 0;
    bool tx_loaded_ = false;
    unsigned wrap_clocks_ = 0;
    uint8_t rx_ = 0;
    bool rx_full_ = false;
    bool rx_parity_ = false;
    uint64_t wrapped_ = 0;
};

} // namespace em34xx
