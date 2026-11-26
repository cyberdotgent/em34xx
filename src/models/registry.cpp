// SPDX-License-Identifier: GPL-3.0-or-later
#include "models/model.hpp"

#include "machine/machine.hpp"
#include "util/strings.hpp"

namespace em34xx {

std::string Model::can_build() const {
    if (info().status == ModelStatus::Placeholder)
        return "model " + info().name + " is a placeholder and cannot run yet";
    return {};
}

const ModelVariant* Model::find_variant(const std::string& id) const {
    const std::string want = to_lower(id);
    const std::string full = to_lower(info().name) + "-" + want;
    for (const ModelVariant& v : info().variants) {
        const std::string have = to_lower(v.id);
        if (have == want || have == full) return &v;
    }
    return nullptr;
}

const ModelVariant& Model::variant_for(const TerminalOptions& options) const {
    if (const ModelVariant* v = options.variant.empty() ? nullptr : find_variant(options.variant)) return *v;
    return default_variant();
}

const ModelVariant& Model::variant_named(const std::string& id) const {
    if (const ModelVariant* v = id.empty() ? nullptr : find_variant(id)) return *v;
    return default_variant();
}

std::string Model::storage_name(const ModelVariant& variant) const {
    return variant.roms.empty() ? info().name : variant.id;
}

const ModelVariant* Model::variant_with_colour(bool colour) const {
    for (const ModelVariant& v : info().variants)
        if (v.colour == colour) return &v;
    return nullptr;
}

const std::vector<const Model*>& all_models() {
    static const std::vector<const Model*> models = {&model_3477(), &model_3483(), &model_3486(), &model_3487(), &model_3489()};
    return models;
}

bool find_model_variant(const std::string& name, const Model*& model, const ModelVariant*& variant) {
    std::string n = to_lower(name);
    if (starts_with(n, "ibm-")) n = n.substr(4);
    else if (starts_with(n, "ibm")) n = n.substr(3);
    for (const Model* m : all_models()) {
        const std::string base = to_lower(m->info().name);
        if (n == base) {
            model = m;
            variant = m->info().variants.empty() ? nullptr : &m->default_variant();
            return true;
        }
        if (starts_with(n, base + "-")) {
            const ModelVariant* v = m->find_variant(n);
            if (!v) return false;
            model = m;
            variant = v;
            return true;
        }
    }
    return false;
}

const Model* find_model(const std::string& name) {
    const Model* m = nullptr;
    const ModelVariant* v = nullptr;
    return find_model_variant(name, m, v) ? m : nullptr;
}

void select_legacy_display(const Model& model, DisplaySubtype legacy, std::string& variant, std::string& phosphor) {
    const bool colour = !is_monochrome(legacy);
    const ModelVariant* v = model.variant_with_colour(colour);
    if (!v) v = &model.default_variant();
    variant = v->id;
    phosphor.clear();
    if (!colour)
        if (const Phosphor* p = v->phosphor(display_subtype_name(legacy))) phosphor = p->id;
}

std::vector<const Model*> runnable_models() {
    std::vector<const Model*> out;
    for (const Model* m : all_models())
        if (m->can_build().empty() && m->info().status == ModelStatus::Bringup) out.push_back(m);
    return out;
}

} // namespace em34xx
