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

bool ModelInfo::option_on(const std::string& id, const std::map<std::string, bool>& switches) const {
    for (const ModelOption& o : options) {
        if (o.id != id) continue;
        const auto it = switches.find(id);
        return it == switches.end() ? o.default_on : it->second;
    }
    return false;
}

const char* session_kind_label(SessionKind kind) {
    switch (kind) {
    case SessionKind::Display: return "display";
    case SessionKind::Printer: return "printer";
    case SessionKind::DisplayOrPrinter: return "display or printer";
    }
    return "?";
}

unsigned ModelInfo::max_printer_sessions() const {
    unsigned n = 0;
    for (SessionKind k : sessions) n += k != SessionKind::Display ? 1u : 0u;
    return n;
}

const Phosphor* ModelVariant::phosphor(const std::string& wanted) const {
    if (phosphors.empty()) return nullptr;
    for (const Phosphor& p : phosphors)
        if (to_lower(p.id) == to_lower(wanted)) return &p;
    return &phosphors.front();
}

std::string variant_label(const ModelInfo& model, const ModelVariant& variant) {
    const char* protocol = model.line == LineKind::Coax ? "3270" : "5250";
    return std::string(protocol) + ", " + variant.description;
}

} // namespace em34xx
