// Terminal model definitions. Each model states its CPU, address space,
// ROM set and memory/device map, and builds a Machine from that. ModelInfo
// itself lives in machine/model_info.hpp, so the machine core does not
// depend on the model registry.
//
// Every hardware fact a model encodes should be traceable to an observation
// (board, datasheet, documentation) or be explicitly marked as a hypothesis
// in its notes; see docs/models.md.
#pragma once

#include "machine/model_info.hpp"
#include "rom/romset.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tn34xx {

class Machine;

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
// Models an end user can run: buildable, with a complete ROM dump known
// (not RomIncomplete, not a placeholder). What a GUI offers.
std::vector<const Model*> runnable_models();

// Individual model singletons (defined in model_*.cpp).
const Model& model_3477();
const Model& model_3486();
const Model& model_3489();

} // namespace tn34xx
