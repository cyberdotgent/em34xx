// Terminal model definitions. Each model states its CPU, address space,
// ROM set and memory/device map, and builds a Machine from that.
//
// Every hardware fact a model encodes should be traceable to an observation
// (board, datasheet, documentation) or be explicitly marked as a hypothesis
// in its notes; see docs/models.md.
#pragma once

#include "cpu/i86.hpp"
#include "rom/romset.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tn34xx {

class Machine;

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
    unsigned max_sessions = 1;   // twinax station addresses (sessions) the unit can answer on
    std::vector<RomSpec> roms;
    std::vector<KnownBadRom> known_bad_roms;
    std::vector<std::string> notes;  // hypotheses and open questions
};

class Model {
public:
    virtual ~Model() = default;
    virtual const ModelInfo& info() const = 0;
    // Returns an empty string if this model can be built at all.
    virtual std::string can_build() const;
    // Map memory and devices. ROM images have already been verified.
    virtual void build(Machine& m, std::vector<LoadedRom>& roms) const = 0;
};

const std::vector<const Model*>& all_models();
const Model* find_model(const std::string& name);

// Individual model singletons (defined in model_*.cpp).
const Model& model_3477();
const Model& model_3486();
const Model& model_3489();

} // namespace tn34xx
