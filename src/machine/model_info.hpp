// SPDX-License-Identifier: GPL-3.0-or-later
// Static description of a terminal model: CPU, address space, ROM set and
// notes. The Machine core needs this; the Model registry that builds
// machines from it lives in src/models (tn34xx_machines).
#pragma once

#include "cpu/cpu.hpp"
#include "rom/romset.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tn34xx {

enum class ModelStatus {
    Bringup,        // ROMs good, hardware being reverse engineered
    RomIncomplete,  // only a defective ROM dump is available
    Placeholder,    // registered so the CLI knows it; cannot run yet
};

const char* model_status_name(ModelStatus s);

struct ModelInfo {
    std::string name;            // "3477"
    std::string description;
    ModelStatus status = ModelStatus::Placeholder;
    CpuLevel cpu = CpuLevel::I8088;
    unsigned address_bits = 20;
    uint64_t clock_hz = 0;       // nominal CPU clock, used for pacing
    unsigned max_sessions = 1;   // twinax station addresses (sessions) the unit can answer on;
                                 // never more than TwinaxConfig::kMaxStationsPerBus (7)
    unsigned max_printer_sessions = 0;  // of those, how many may be printer sessions
    std::vector<RomSpec> roms;
    std::vector<KnownBadRom> known_bad_roms;
    std::vector<std::string> notes;  // hypotheses and open questions
};

} // namespace tn34xx
