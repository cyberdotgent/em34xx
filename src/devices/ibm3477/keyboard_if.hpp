// SPDX-License-Identifier: GPL-3.0-or-later
// IBM 3477 keyboard interface (gate array registers 20012-20014, interrupt
// enable 20007 bit 0, vector 0Eh), connected to a set-3 terminal keyboard.
//
// The interface is a byte-wide shift register on the keyboard's clock/data
// pair:
//   20012 W  byte to send; R  last byte received
//   20013 W  line control: 03 = line held (idle; the keyboard cannot
//            send), 02 = line released: a byte written to 20012 is sent,
//            and the keyboard may send to the host. While bit 0 is set,
//            each 0->1 transition of bit 1 clocks the shift register by
//            hand; after 11 clocks (start, 8 data, parity, stop) the byte
//            written to 20012 has looped back into the receive side (wrap
//            test) [observed from the firmware's self-test; the loop-back
//            path is a hypothesis].
//   20013 R  status: bit 0 = received odd-parity bit, bit 1 = busy
//            sending, bit 2 = byte received
//   20014 W  clear the receive status and the interrupt
// A received byte raises the keyboard interrupt, and so does the end of a
// transmission to the keyboard [observed: the firmware waits for one
// interrupt per byte sent and discards it]. Either is cleared by 20014; the
// keyboard cannot send while one is pending.
#pragma once

#include "input/set3_keyboard.hpp"
#include "machine/peripherals.hpp"

#include <cstdint>
#include <string>

namespace em34xx {

class Ibm3477Interrupts;

class Ibm3477KeyboardInterface : public KeyboardSink {
public:
    // About 11 bits at the keyboard's 10-16.7 kHz clock.
    static constexpr unsigned kByteTimeUs = 1000;

    Ibm3477KeyboardInterface(uint64_t clock_hz, KeyboardType type, uint8_t id1, uint8_t id2);

    void connect(Ibm3477Interrupts* ic) { ic_ = ic; }
    Set3Keyboard& keyboard() { return kbd_; }

    // Register access; reg 0 = 20012, 1 = 20013, 2 = 20014.
    uint8_t read(unsigned reg) const;
    void write(unsigned reg, uint8_t value);

    void reset();
    void tick(uint64_t cycles);

    void host_key(const HostKeyEvent& ev) override { kbd_.host_key(ev); }
    bool terminal_key(const std::string& function, bool pressed) override {
        return kbd_.terminal_key(function, pressed);
    }
    bool set_host_keymap(const std::string& spec, bool shift_fkeys, std::string& error) override {
        return kbd_.set_host_keymap(spec, shift_fkeys, error);
    }

    uint8_t status() const;
    bool interrupt_pending() const { return rx_full_ || tx_done_; }
    uint64_t bytes_sent() const { return sent_; }
    uint64_t bytes_received() const { return received_; }
    std::string describe() const;

private:
    void set_rx(uint8_t byte);

    Set3Keyboard kbd_;
    Ibm3477Interrupts* ic_ = nullptr;
    uint64_t clock_hz_;
    uint64_t byte_cycles_;
    uint64_t us_acc_ = 0;      // cycle remainder for keyboard time

    uint8_t ctl_ = 0x03;
    uint8_t tx_ = 0;
    bool tx_loaded_ = false;   // 20012 written, not yet sent
    uint64_t tx_left_ = 0;     // cycles until the byte reaches the keyboard
    unsigned wrap_clocks_ = 0;

    uint8_t rx_ = 0;
    bool rx_full_ = false;
    bool tx_done_ = false;     // transmit-complete interrupt pending
    bool rx_parity_ = false;
    uint64_t rx_left_ = 0;     // cycles until the next keyboard byte arrives
    bool rx_active_ = false;

    uint64_t sent_ = 0, received_ = 0;
};

} // namespace em34xx
