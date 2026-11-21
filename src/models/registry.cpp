// SPDX-License-Identifier: GPL-3.0-or-later
#include "models/model.hpp"

#include "util/strings.hpp"

namespace em34xx {

std::string Model::can_build() const {
    if (info().status == ModelStatus::Placeholder)
        return "model " + info().name + " is a placeholder and cannot run yet";
    return {};
}

const std::vector<const Model*>& all_models() {
    static const std::vector<const Model*> models = {&model_3477(), &model_3486(), &model_3489()};
    return models;
}

const Model* find_model(const std::string& name) {
    std::string n = to_lower(name);
    if (starts_with(n, "ibm-")) n = n.substr(4);
    else if (starts_with(n, "ibm")) n = n.substr(3);
    for (const Model* m : all_models())
        if (to_lower(m->info().name) == n) return m;
    return nullptr;
}

std::vector<const Model*> runnable_models() {
    std::vector<const Model*> out;
    for (const Model* m : all_models())
        if (m->can_build().empty() && m->info().status == ModelStatus::Bringup) out.push_back(m);
    return out;
}

} // namespace em34xx
