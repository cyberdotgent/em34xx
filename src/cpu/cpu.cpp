// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu/cpu.hpp"

#include "cpu/i86.hpp"

namespace em34xx {

std::unique_ptr<Cpu> make_cpu(CpuBus& bus, CpuLevel level) {
    // TODO(80286): return a protected-mode core for CpuLevel::I80286 once
    // there is firmware to bring it up against (the 3489 needs a re-dump).
    return std::make_unique<I86>(bus, level);
}

} // namespace em34xx
