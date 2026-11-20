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

const char* station_type_name(StationType type) {
    switch (type) {
    case StationType::Display: return "display";
    case StationType::Printer: return "printer";
    }
    return "?";
}

bool parse_station_type(const std::string& text, StationType& out) {
    const std::string t = to_lower(text);
    if (t == "display") out = StationType::Display;
    else if (t == "printer") out = StationType::Printer;
    else return false;
    return true;
}

bool parse_station_spec(const std::string& text, StationConfig& out, std::string& error) {
    StationConfig st;
    const auto c1 = text.find(':');
    const std::string addr = text.substr(0, c1);
    uint64_t n;
    if (!parse_uint(addr, n) || n > 6) {
        error = "station address must be 0..6 in '" + text + "'";
        return false;
    }
    st.address = unsigned(n);
    if (c1 != std::string::npos) {
        const auto c2 = text.find(':', c1 + 1);
        const std::string type = text.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        if (!parse_station_type(type, st.type)) {
            error = "station type must be display or printer in '" + text + "'";
            return false;
        }
        if (c2 != std::string::npos) st.device_name = text.substr(c2 + 1);
    }
    out = st;
    return true;
}

std::vector<StationConfig> TwinaxConfig::effective_stations() const {
    if (!stations.empty()) return stations;
    StationConfig st;
    st.address = station_address;
    return {st};
}

std::string TwinaxConfig::validate(unsigned max_sessions) const {
    if (station_address > 6) return "twinax station address must be 0..6";
    if (!stations.empty() && station_address != 0)
        return "use either a station list or the single station address, not both";
    const std::vector<StationConfig> list = effective_stations();
    unsigned seen = 0;
    for (const StationConfig& st : list) {
        if (st.address > 6) return format("twinax station address %u is out of range 0..6", st.address);
        if (seen & (1u << st.address)) return format("twinax station address %u is configured twice", st.address);
        seen |= 1u << st.address;
        if (st.device_name.size() > 10)
            return format("station %u: tn5250 device name is longer than 10 characters", st.address);
    }
    if (max_sessions && list.size() > max_sessions)
        return format("%zu twinax stations configured, but this terminal supports at most %u session(s)",
                      list.size(), max_sessions);
    return {};
}

} // namespace tn34xx
