// SPDX-License-Identifier: GPL-3.0-or-later
// A standard two-button PS/2 mouse (device side of the protocol): commands
// from the host, acknowledgements, the power-on self-test report and
// three-byte movement packets in stream mode. Bit-level framing is the
// port's business; this class speaks bytes.
//
// Packet: byte 1 = YO XO YS XS 1 M R L, byte 2 = X movement, byte 3 = Y
// movement (positive is up), both 9-bit two's complement with the sign in
// byte 1.
#pragma once

#include <cstdint>
#include <deque>
#include <string>

namespace em34xx {

class Ps2Mouse {
public:
    static constexpr unsigned kLeft = 1, kRight = 2, kMiddle = 4;

    void power_on();

    // Host -> mouse.
    void host_write(uint8_t byte);
    // Mouse -> host.
    bool has_output() const { return !out_.empty(); }
    uint8_t read_output();

    // The user moved the mouse (counts; dy positive = towards the user, as
    // host window systems report it) or changed the buttons.
    void move(int dx, int dy, unsigned buttons);

    bool reporting() const { return enabled_ && !remote_; }
    uint64_t packets() const { return packets_; }
    std::string describe() const;

private:
    void reply(uint8_t b) { out_.push_back(b); last_ = b; }
    void defaults();
    void queue_packet();

    std::deque<uint8_t> out_;
    uint8_t last_ = 0;
    uint8_t pending_cmd_ = 0;   // command waiting for its argument byte
    bool enabled_ = false;
    bool remote_ = false;
    bool scaling_21_ = false;
    uint8_t rate_ = 100;
    uint8_t resolution_ = 2;
    int acc_x_ = 0, acc_y_ = 0;
    unsigned buttons_ = 0;
    uint64_t packets_ = 0;
    uint64_t commands_ = 0;
};

} // namespace em34xx
