// SPDX-License-Identifier: GPL-3.0-or-later
#include "machine/twinax_port.hpp"

#include "util/strings.hpp"

namespace em34xx {

namespace {

unsigned popcount16(uint16_t v) {
    unsigned n = 0;
    for (; v; v &= uint16_t(v - 1)) ++n;
    return n;
}

} // namespace

uint16_t TwinaxFrame::encode() const {
    uint16_t w = 0x8000;  // sync
    for (unsigned i = 0; i < 8; ++i)
        if (data & (1u << i)) w |= uint16_t(1u << (14 - i));   // LSB first
    for (unsigned i = 0; i < 3; ++i)
        if (address & (1u << i)) w |= uint16_t(1u << (6 - i));  // LSB first
    if (popcount16(w) & 1) w |= 0x0008;                       // even parity
    return w;
}

bool TwinaxFrame::decode(uint16_t word, TwinaxFrame& out, bool& parity_ok) {
    if (!(word & 0x8000)) return false;
    uint8_t d = 0;
    for (unsigned i = 0; i < 8; ++i)
        if (word & (1u << (14 - i))) d = uint8_t(d | (1u << i));
    out.data = d;
    uint8_t a = 0;
    for (unsigned i = 0; i < 3; ++i)
        if (word & (1u << (6 - i))) a = uint8_t(a | (1u << i));
    out.address = a;
    parity_ok = (popcount16(uint16_t(word & 0xFFF8u)) & 1) == 0;
    return true;
}

std::string format_twinax_message(const TwinaxMessage& msg) {
    std::string out;
    for (size_t i = 0; i < msg.size(); ++i)
        out += format(i ? " %02x/%u" : "%02x/%u", msg[i].data, msg[i].address);
    return out;
}

} // namespace em34xx
