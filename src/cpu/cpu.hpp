// CPU-independent definitions shared by the CPU cores and the machine.
#pragma once

#include <string>

namespace tn34xx {

enum class CpuLevel {
    I8086,
    I8088,
    I80186,
    I80188,
    I80286,
};

const char* cpu_level_name(CpuLevel level);
bool parse_cpu_level(const std::string& text, CpuLevel& out);

} // namespace tn34xx
