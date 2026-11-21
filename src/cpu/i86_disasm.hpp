// SPDX-License-Identifier: GPL-3.0-or-later
// Intel-syntax disassembler for the 8086/80186 instruction set.
#pragma once

#include "cpu/i86.hpp"

#include <functional>
#include <string>

namespace em34xx {

struct DisasmResult {
    std::string text;      // e.g. "mov word [es:bx+0x10], ax"
    unsigned length = 0;   // instruction length in bytes, prefixes included
    // Control-flow hints for "step over" / gdb.
    bool is_call = false;
    bool is_return = false;
};

// read(linear) returns the byte at a linear address without side effects.
using DisasmReader = std::function<uint8_t(uint32_t)>;

DisasmResult disassemble(const DisasmReader& read, uint16_t cs, uint16_t ip, CpuLevel level,
                         uint32_t address_mask = 0xFFFFF);

} // namespace em34xx
