// SPDX-License-Identifier: GPL-3.0-or-later
#include "machine/coax_port.hpp"

#include "util/strings.hpp"

namespace em34xx {

std::string format_coax_message(const CoaxMessage& msg) {
    std::string s;
    for (const CoaxWord& w : msg) {
        if (!s.empty()) s += ' ';
        if (w.is_turnaround()) s += "TT/AR";
        else if (w.is_command()) s += format("cmd %02X", w.byte());
        else s += format("%02X", w.byte());
    }
    return s;
}

} // namespace em34xx
