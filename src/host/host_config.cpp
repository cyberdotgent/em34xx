// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/host_config.hpp"

#include "util/fingerprint.hpp"
#include "util/strings.hpp"

namespace em34xx {

std::string Tn5250Config::validate() const {
    if (!configured()) return {};
    if (port == 0) return "tn5250 port must be non-zero";
    if (device_name.size() > 10) return "tn5250 device name is longer than 10 characters";
    if (!tls && (!ca_file.empty())) return "tn5250 ca_file is set but tls is disabled";
    if (!tls_cert_sha256.empty()) {
        Sha256Digest d;
        if (!parse_sha256_fingerprint(tls_cert_sha256, d))
            return "tn5250 tls_cert_sha256 must be 32 hex bytes, e.g. AB:CD:...; got '" + tls_cert_sha256 + "'";
        if (!tls) return "tn5250 tls_cert_sha256 is set but tls is disabled";
        if (tls_verify == TlsVerify::None)
            return "tn5250 tls_cert_sha256 and tls_verify = \"none\" contradict each other; drop one";
    }
    if (tls_min_version == TlsVersion::Any) return "tn5250 tls_min_version must be ssl3, 1.0, 1.1, 1.2 or 1.3";
    if (tls_max_version != TlsVersion::Any && tls_max_version < tls_min_version)
        return format("tn5250 tls_min_version %s is newer than tls_max_version %s", tls_version_name(tls_min_version),
                      tls_version_name(tls_max_version));
    for (const auto& kv : env)
        if (kv.first.empty()) return "tn5250 environment variable with empty name";
    return {};
}

std::string derived_terminal_type(const std::string& model, DisplaySubtype display) {
    const bool colour = !is_monochrome(display);
    if (model == "3477") return colour ? "IBM-3477-FC" : "IBM-3477-FG";
    return colour ? "IBM-3179-2" : "IBM-5251-11";
}

std::string station_terminal_type(const Tn5250Config& tn5250, const StationConfig& station,
                                  const std::string& model, DisplaySubtype display) {
    if (!station.profile.empty()) return station.profile;
    if (!tn5250.terminal_type.empty()) return tn5250.terminal_type;
    return derived_terminal_type(model, display);
}

std::string Tn5250Config::describe() const {
    if (!configured()) return "tn5250: not configured\n";
    std::string out = format("tn5250: %s:%u%s\n", host.c_str(), port, tls ? " (TLS)" : "");
    out += format("  terminal type %s, device name %s, codepage %s\n", terminal_type.empty() ? "(derived from model and display)" : terminal_type.c_str(),
                  device_name.empty() ? "(host assigned)" : device_name.c_str(), codepage.c_str());
    if (tls) {
        std::string how = tls_cert_sha256.empty() ? tls_verify_name(tls_verify) : "pinned " + tls_cert_sha256;
        if (!ca_file.empty()) how += ", CA " + ca_file;
        out += format("  verify %s\n", how.c_str());
        out += format("  protocol %s to %s\n", tls_version_name(tls_min_version),
                      tls_max_version == TlsVersion::Any ? "newest" : tls_version_name(tls_max_version));
    }
    for (const auto& kv : env) out += format("  env %s=%s\n", kv.first.c_str(), kv.second.c_str());
    return out;
}

const char* tls_verify_name(TlsVerify v) {
    switch (v) {
    case TlsVerify::System: return "system";
    case TlsVerify::None: return "none";
    }
    return "?";
}

bool parse_tls_verify(const std::string& text, TlsVerify& out) {
    const std::string t = to_lower(trim(text));
    if (t == "system" || t == "default" || t == "on" || t == "true" || t == "yes") out = TlsVerify::System;
    else if (t == "none" || t == "insecure" || t == "off" || t == "false" || t == "no") out = TlsVerify::None;
    else return false;
    return true;
}

const char* tls_version_name(TlsVersion v) {
    switch (v) {
    case TlsVersion::Any: return "any";
    case TlsVersion::Ssl3: return "ssl3";
    case TlsVersion::Tls1_0: return "1.0";
    case TlsVersion::Tls1_1: return "1.1";
    case TlsVersion::Tls1_2: return "1.2";
    case TlsVersion::Tls1_3: return "1.3";
    }
    return "?";
}

bool parse_tls_version(const std::string& text, TlsVersion& out) {
    std::string t = to_lower(trim(text));
    if (t.empty() || t == "any") { out = TlsVersion::Any; return true; }
    if (t == "ssl3" || t == "sslv3" || t == "ssl3.0" || t == "3.0") { out = TlsVersion::Ssl3; return true; }
    if (starts_with(t, "tlsv")) t = t.substr(4);
    else if (starts_with(t, "tls")) t = t.substr(3);
    if (t == "1" || t == "1.0" || t == "10") out = TlsVersion::Tls1_0;
    else if (t == "1.1" || t == "11") out = TlsVersion::Tls1_1;
    else if (t == "1.2" || t == "12") out = TlsVersion::Tls1_2;
    else if (t == "1.3" || t == "13") out = TlsVersion::Tls1_3;
    else return false;
    return true;
}

void apply_station_tls(Tn5250Config& cfg, const StationConfig& station) {
    TlsVersion v;
    if (!station.tls_min_version.empty() && parse_tls_version(station.tls_min_version, v)) cfg.tls_min_version = v;
    if (!station.tls_max_version.empty() && parse_tls_version(station.tls_max_version, v)) cfg.tls_max_version = v;
    if (!station.tls_cert_sha256.empty()) {
        cfg.tls_cert_sha256 = station.tls_cert_sha256;
        cfg.tls_verify = TlsVerify::System;   // a pin decides alone
    }
}

const char* twinax_host_kind_name(TwinaxHostKind kind) {
    switch (kind) {
    case TwinaxHostKind::None: return "none";
    case TwinaxHostKind::Script: return "script";
    case TwinaxHostKind::Tn5250: return "tn5250";
    case TwinaxHostKind::Wsc: return "wsc";
    }
    return "?";
}

bool parse_twinax_host_kind(const std::string& text, TwinaxHostKind& out) {
    const std::string t = to_lower(text);
    if (t == "none") out = TwinaxHostKind::None;
    else if (t == "script") out = TwinaxHostKind::Script;
    else if (t == "tn5250") out = TwinaxHostKind::Tn5250;
    else if (t == "wsc") out = TwinaxHostKind::Wsc;
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

std::string TwinaxConfig::validate(unsigned max_sessions, int max_printers) const {
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
        TlsVersion v;
        if (!st.tls_min_version.empty() && (!parse_tls_version(st.tls_min_version, v) || v == TlsVersion::Any))
            return format("station %u: tls_min_version must be ssl3, 1.0, 1.1, 1.2 or 1.3", st.address);
        if (!st.tls_max_version.empty() && !parse_tls_version(st.tls_max_version, v))
            return format("station %u: tls_max_version must be ssl3, 1.0, 1.1, 1.2, 1.3 or any", st.address);
        Sha256Digest d;
        if (!st.tls_cert_sha256.empty() && !parse_sha256_fingerprint(st.tls_cert_sha256, d))
            return format("station %u: tls_cert_sha256 is not a SHA-256 fingerprint", st.address);
        if (st.type != StationType::Display && (!st.host.empty() || st.port || !st.tls_cert_sha256.empty()))
            return format("station %u: host, port and tls_cert_sha256 apply to display stations only", st.address);
    }
    if (list.size() > kMaxStationsPerBus)
        return format("%zu twinax stations configured, but a cable carries at most %u",
                      list.size(), kMaxStationsPerBus);
    if (max_sessions && list.size() > max_sessions)
        return format("%zu twinax stations configured, but this terminal supports at most %u session(s)",
                      list.size(), max_sessions);
    unsigned printers = 0;
    for (const StationConfig& st : list) printers += st.type == StationType::Printer ? 1u : 0u;
    if (max_printers >= 0 && printers > unsigned(max_printers))
        return max_printers == 0 ? std::string("this terminal has no printer session")
                                 : format("%u printer stations configured, but this terminal supports at most %d",
                                          printers, max_printers);
    return {};
}

} // namespace em34xx
