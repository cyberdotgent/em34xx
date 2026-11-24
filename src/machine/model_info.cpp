// SPDX-License-Identifier: GPL-3.0-or-later
#include "machine/model_info.hpp"

#include "util/strings.hpp"

namespace em34xx {

const char* model_status_name(ModelStatus s) {
    switch (s) {
    case ModelStatus::Bringup: return "bring-up";
    case ModelStatus::RomIncomplete: return "ROM incomplete";
    case ModelStatus::Placeholder: return "placeholder";
    }
    return "?";
}

const Phosphor& phosphor_green() {
    static const Phosphor p{"green", "Green", 0xFF66FFB0u};   // mint green, measured on a real 3486 tube
    return p;
}
const Phosphor& phosphor_amber() {
    static const Phosphor p{"amber", "Amber", 0xFFFFB000u};   // P134-like amber
    return p;
}
const Phosphor& phosphor_paper_white() {
    static const Phosphor p{"paperwhite", "Paper white", 0xFFE8E8DCu};   // P4-like, slightly warm
    return p;
}

const Phosphor* ModelVariant::phosphor(const std::string& wanted) const {
    if (phosphors.empty()) return nullptr;
    for (const Phosphor& p : phosphors)
        if (to_lower(p.id) == to_lower(wanted)) return &p;
    return &phosphors.front();
}

} // namespace em34xx
