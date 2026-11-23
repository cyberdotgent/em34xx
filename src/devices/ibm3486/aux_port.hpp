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
//   20145 R  bit 0 = received odd-parity bit, bit 1 = busy sending, bit 2 =
//            byte received, as on the keyboard port [observed: the
//            firmware's receive handler]; bit 7 selects the board's
//            interrupt layout and reads 0 here [hypothesis]
//   20146 W  clear the receive status and the interrupt
// With the line released a byte written to 20144 is sent to the device, and
// the device may send. As on the keyboard port, the end of a transmission
// interrupts like a received byte [hypothesis: observed for the keyboard
// port only]. Without a device nothing answers, as with an empty jack. A
// PS/2 mouse can be attached (input/ps2_mouse.hpp).
#pragma once

#include "bus/device.hpp"
#include "input/ps2_mouse.hpp"
#include "machine/peripherals.hpp"

#include <cstdint>
#include <string>

namespace em34xx {

class Ibm3477Interrupts;

class Ibm3486AuxPort : public Device, public PointerSink {
public:
    // About 11 bits at the device's 10-16.7 kHz clock.
    static constexpr unsigned kByteTimeUs = 1000;

    explicit Ibm3486AuxPort(uint64_t clock_hz = 7500000)
        : Device("aux-port"), byte_cycles_((clock_hz ? clock_hz : 1000000) * kByteTimeUs / 1000000) {}

    // Plug a mouse in (before power-on) or leave the jack empty.
    void attach_mouse(bool on) { has_mouse_ = on; }
    bool has_mouse() const { return has_mouse_; }
    Ps2Mouse& mouse() { return mouse_; }
    void pointer_event(int dx, int dy, unsigned buttons) override {
        if (has_mouse_) mouse_.move(dx, dy, buttons);
    }

    void connect(Ibm3477Interrupts* ic) { ic_ = ic; }

    // Register access; reg 0 = 20144, 1 = 20145, 2 = 20146.
    uint8_t read(unsigned reg) const;
    void write(unsigned reg, uint8_t value);

    void reset() override;
    void tick(uint64_t cycles) override;
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

    bool has_mouse_ = false;
    Ps2Mouse mouse_;
    uint64_t byte_cycles_;
    uint64_t tx_left_ = 0;     // cycles until the byte reaches the device
    bool tx_done_ = false;     // transmit-complete interrupt pending
    uint64_t rx_left_ = 0;     // cycles until the device's next byte arrives
    bool rx_active_ = false;
    uint64_t sent_ = 0, received_ = 0;
};

} // namespace em34xx
