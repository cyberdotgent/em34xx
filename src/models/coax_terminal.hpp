// SPDX-License-Identifier: GPL-3.0-or-later
// The coax (CUT) terminals' common board: the display controller, coax
// adapter, system control and character-generator window of
// hw/board_3483.hpp, with the keyboard on serial channel A. The 3483 and
// the 3472 build it at their own addresses (CoaxBoardLayout).
#pragma once

#include "hw/board_3483.hpp"

#include <cstdint>

namespace em34xx {

class Machine;
class I80186Peripherals;
struct ModelInfo;

// Adds the board's devices to `m` and maps them at `layout`. `cpu_io` is
// the 80186's peripheral block, or null for an 8086 (the board is then the
// bus's interrupt controller). `sense` is the monitor sense code, or the
// display ID on a board without monitor sense lines
// (CoaxBoardLayout::monitor_sense_lines).
void build_coax_terminal(Machine& m, const ModelInfo& info, const CoaxBoardLayout& layout, I80186Peripherals* cpu_io,
                         uint8_t sense);

} // namespace em34xx
