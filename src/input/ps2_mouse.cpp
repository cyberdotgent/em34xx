// SPDX-License-Identifier: GPL-3.0-or-later
#include "input/ps2_mouse.hpp"

#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>

namespace em34xx {

namespace {
constexpr uint8_t kAck = 0xFA;
constexpr uint8_t kResend = 0xFE;
constexpr uint8_t kSelfTestOk = 0xAA;
constexpr uint8_t kMouseId = 0x00;
constexpr size_t kMaxQueue = 48;   // do not pile up packets nobody reads
} // namespace

void Ps2Mouse::defaults() {
    enabled_ = false;
    remote_ = false;
    scaling_21_ = false;
    rate_ = 100;
    resolution_ = 2;
    acc_x_ = acc_y_ = 0;
}

void Ps2Mouse::power_on() {
    out_.clear();
    pending_cmd_ = 0;
    defaults();
    buttons_ = 0;
    // Power-on self test passed, then the device ID.
    reply(kSelfTestOk);
    reply(kMouseId);
}

uint8_t Ps2Mouse::read_output() {
    const uint8_t b = out_.front();
    out_.pop_front();
    return b;
}

void Ps2Mouse::queue_packet() {
    const int dx = std::max(-255, std::min(255, acc_x_));
    const int dy = std::max(-255, std::min(255, acc_y_));
    acc_x_ -= dx;
    acc_y_ -= dy;
    uint8_t b0 = uint8_t(0x08 | (buttons_ & 7));
    if (dx < 0) b0 |= 0x10;
    if (dy < 0) b0 |= 0x20;
    out_.push_back(b0);
    out_.push_back(uint8_t(dx));
    out_.push_back(uint8_t(dy));
    ++packets_;
}

void Ps2Mouse::move(int dx, int dy, unsigned buttons) {
    const bool changed = dx || dy || (buttons & 7) != buttons_;
    acc_x_ += dx;
    acc_y_ -= dy;   // the protocol counts upwards movement as positive
    buttons_ = buttons & 7;
    if (!changed || !reporting()) return;
    if (out_.size() + 3 > kMaxQueue) return;   // movement stays accumulated
    queue_packet();
}

void Ps2Mouse::host_write(uint8_t b) {
    ++commands_;
    if (pending_cmd_) {
        const uint8_t cmd = pending_cmd_;
        pending_cmd_ = 0;
        if (cmd == 0xF3) rate_ = b;
        else if (cmd == 0xE8) resolution_ = uint8_t(b & 3);
        LOG_DEBUG("mouse", "command %02X %02X", cmd, b);
        reply(kAck);
        return;
    }
    LOG_DEBUG("mouse", "command %02X", b);
    switch (b) {
    case 0xFF:   // reset
        out_.clear();
        defaults();
        reply(kAck);
        reply(kSelfTestOk);
        reply(kMouseId);
        break;
    case 0xFE:   // resend
        out_.push_back(last_);
        break;
    case 0xF6:   // set defaults
        defaults();
        reply(kAck);
        break;
    case 0xF5:   // disable data reporting
        enabled_ = false;
        reply(kAck);
        break;
    case 0xF4:   // enable data reporting
        enabled_ = true;
        acc_x_ = acc_y_ = 0;
        reply(kAck);
        break;
    case 0xF3:   // set sample rate (argument follows)
    case 0xE8:   // set resolution (argument follows)
        pending_cmd_ = b;
        reply(kAck);
        break;
    case 0xF2:   // get device ID
        reply(kAck);
        reply(kMouseId);
        break;
    case 0xF0:   // remote mode
        remote_ = true;
        reply(kAck);
        break;
    case 0xEA:   // stream mode
        remote_ = false;
        reply(kAck);
        break;
    case 0xEB:   // read data
        reply(kAck);
        queue_packet();
        break;
    case 0xE9:   // status request
        reply(kAck);
        reply(uint8_t((remote_ ? 0x40 : 0) | (enabled_ ? 0x20 : 0) | (scaling_21_ ? 0x10 : 0) |
                      ((buttons_ & kLeft) ? 0x04 : 0) | ((buttons_ & kMiddle) ? 0x02 : 0) |
                      ((buttons_ & kRight) ? 0x01 : 0)));
        reply(resolution_);
        reply(rate_);
        break;
    case 0xE7:   // scaling 2:1
    case 0xE6:   // scaling 1:1
        scaling_21_ = b == 0xE7;
        reply(kAck);
        break;
    default:
        reply(kResend);
        break;
    }
}

std::string Ps2Mouse::describe() const {
    return format("PS/2 mouse: %s%s, rate %u, resolution %u, buttons %u, %llu command byte(s), %llu packet(s), "
                  "%zu byte(s) queued",
                  enabled_ ? "reporting" : "not reporting", remote_ ? " (remote mode)" : "", rate_, resolution_,
                  buttons_, (unsigned long long)commands_, (unsigned long long)packets_, out_.size());
}

} // namespace em34xx
