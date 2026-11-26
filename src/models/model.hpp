// SPDX-License-Identifier: GPL-3.0-or-later
// Terminal model definitions. Each model states its CPU, address space,
// ROM set and memory/device map, and builds a Machine from that. ModelInfo
// itself lives in machine/model_info.hpp, so the machine core does not
// depend on the model registry.
//
// Every hardware fact a model encodes should be traceable to an observation
// (board, datasheet, documentation) or be explicitly marked as a hypothesis
// in its notes; see docs/models.md.
#pragma once

#include "machine/display_subtype.hpp"
#include "machine/model_info.hpp"
#include "rom/romset.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace em34xx {

class Machine;
struct TerminalOptions;

class Model {
public:
    virtual ~Model() = default;
    virtual const ModelInfo& info() const = 0;
    // Returns an empty string if this model can be built at all.
    virtual std::string can_build() const;
    // Map memory and devices. ROM images have already been verified.
    virtual void build(Machine& m, std::vector<LoadedRom>& roms) const = 0;

    // The variant the options select (TerminalOptions::variant), or the
    // model's first when they name none or one it does not have.
    const ModelVariant& variant_for(const TerminalOptions& options) const;
    // By id ("3477-FG"; the suffix alone, "FG", also matches); null when
    // the model has no such variant.
    const ModelVariant* find_variant(const std::string& id) const;
    const ModelVariant& default_variant() const { return info().variants.front(); }
    // find_variant(), or the model's first for "" and unknown ids.
    const ModelVariant& variant_named(const std::string& id) const;
    // The name a unit's saved state (battery-backed RAM, EEPROM) is kept
    // under: the model's, so its variants share one setup; the variant's
    // own id when it runs its own firmware, which keeps the setup in its
    // own format (observed: the colour 3477 resets a monochrome 3477's).
    std::string storage_name(const ModelVariant& variant) const;
    // The first variant that is colour (or is not); null when there is none.
    const ModelVariant* variant_with_colour(bool colour) const;
};

const std::vector<const Model*>& all_models();
// By model name ("3477", "IBM-3477") or by variant id ("3477-FG").
const Model* find_model(const std::string& name);
// The same, also giving the variant named (the model's first for a bare
// model name). False when there is no such model or variant.
bool find_model_variant(const std::string& name, const Model*& model, const ModelVariant*& variant);

// The settings of older versions, which had one "display subtype" (green,
// amber, paper-white, colour) per terminal: the variant and phosphor that
// selection means for `model` now. Colour on a model without a colour
// variant gives its first variant.
void select_legacy_display(const Model& model, DisplaySubtype legacy, std::string& variant, std::string& phosphor);
// Models an end user can run: buildable, with a complete ROM dump known
// (not RomIncomplete, not a placeholder). What a GUI offers.
std::vector<const Model*> runnable_models();

// Individual model singletons (defined in model_*.cpp).
const Model& model_3477();
const Model& model_3472();   // 3270 coax, monochrome; bring-up skeleton
const Model& model_3483();   // 3270 coax; bring-up skeleton
const Model& model_3486();
const Model& model_3487();   // same board and firmware as the 3486
const Model& model_3489();

} // namespace em34xx
