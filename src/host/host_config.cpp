#include "host/host_config.hpp"

#include "util/strings.hpp"

namespace tn34xx {

std::string Tn5250Config::validate() const {
    if (!configured()) return {};
    if (port == 0) return "tn5250 port must be non-zero";
    if (device_name.size() > 10) return "tn5250 device name is longer than 10 characters";
    if (!tls && (!ca_file.empty())) return "tn5250 ca_file is set but tls is disabled";
    for (const auto& kv : env)
        if (kv.first.empty()) return "tn5250 environment variable with empty name";
    return {};
}

std::string Tn5250Config::describe() const {
    if (!configured()) return "tn5250: not configured\n";
    std::string out = format("tn5250: %s:%u%s\n", host.c_str(), port, tls ? " (TLS)" : "");
    out += format("  terminal type %s, device name %s, codepage %s\n", terminal_type.c_str(),
                  device_name.empty() ? "(host assigned)" : device_name.c_str(), codepage.c_str());
    if (tls) out += format("  verify %s%s\n", tls_verify ? "on" : "off",
                           ca_file.empty() ? "" : (", CA " + ca_file).c_str());
    for (const auto& kv : env) out += format("  env %s=%s\n", kv.first.c_str(), kv.second.c_str());
    return out;
}

const char* twinax_host_kind_name(TwinaxHostKind kind) {
    switch (kind) {
    case TwinaxHostKind::None: return "none";
    case TwinaxHostKind::Script: return "script";
    case TwinaxHostKind::Tn5250: return "tn5250";
    }
    return "?";
}

bool parse_twinax_host_kind(const std::string& text, TwinaxHostKind& out) {
    const std::string t = to_lower(text);
    if (t == "none") out = TwinaxHostKind::None;
    else if (t == "script") out = TwinaxHostKind::Script;
    else if (t == "tn5250") out = TwinaxHostKind::Tn5250;
    else return false;
    return true;
}

std::string TwinaxConfig::validate() const {
    if (station_address > 6) return "twinax station address must be 0..6";
    return {};
}

} // namespace tn34xx
