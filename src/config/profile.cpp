// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/profile.hpp"

#include "config/settings.hpp"
#include "util/fingerprint.hpp"
#include "util/strings.hpp"

#include <toml++/toml.hpp>

#include <sstream>

namespace em34xx {

const char* profile_tls_mode_name(ProfileTlsMode m) {
    switch (m) {
    case ProfileTlsMode::System: return "system";
    case ProfileTlsMode::AcceptAll: return "none";
    case ProfileTlsMode::Fingerprint: return "fingerprint";
    }
    return "?";
}

bool parse_profile_tls_mode(const std::string& text, ProfileTlsMode& out) {
    const std::string t = to_lower(trim(text));
    if (t == "system" || t == "on" || t == "verify") out = ProfileTlsMode::System;
    else if (t == "none" || t == "off" || t == "insecure" || t == "accept-all") out = ProfileTlsMode::AcceptAll;
    else if (t == "fingerprint" || t == "sha256" || t == "pin") out = ProfileTlsMode::Fingerprint;
    else return false;
    return true;
}

bool Profile::any_tls() const {
    for (const ProfileSession& s : sessions)
        if (s.used() && s.tls) return true;
    return false;
}

std::string Profile::validate() const {
    if (trim(model).empty()) return "no terminal type";
    for (size_t i = 0; i < kSessions; ++i) {
        const ProfileSession& s = sessions[i];
        const std::string which = format("session %zu", i + 1);
        if (!s.used()) {
            if (i == 0) return "session 1 needs a host name";
            continue;
        }
        if (s.host.find_first_of(" \t\r\n") != std::string::npos) return which + ": the host name contains blanks";
        if (s.port == 0) return which + ": the port must be 1..65535";
        if (s.device_name.size() > 10) return which + ": the device name is longer than 10 characters";
        if (s.terminal_type.find_first_of(" \t\r\n") != std::string::npos)
            return which + ": the terminal type contains blanks";
        if (s.terminal_type.size() > 40) return which + ": the terminal type is longer than 40 characters";
        Sha256Digest d;
        if (!s.cert_sha256.empty() && !parse_sha256_fingerprint(s.cert_sha256, d))
            return which + ": the certificate fingerprint is not a SHA-256 fingerprint";
    }
    Sha256Digest d;
    if (any_tls() && tls_mode == ProfileTlsMode::Fingerprint) {
        if (trim(tls_cert_sha256).empty()) return "TLS: the certificate fingerprint is empty";
        if (!parse_sha256_fingerprint(tls_cert_sha256, d))
            return "TLS: the certificate fingerprint is not a SHA-256 fingerprint (32 hex bytes)";
    }
    if (!tls_min_version.empty()) {
        TlsVersion v;
        if (!parse_tls_version(tls_min_version, v) || v == TlsVersion::Any)
            return "TLS: the minimum version must be ssl3, 1.0, 1.1, 1.2 or 1.3";
    }
    return {};
}

bool Profile::operator==(const Profile& o) const {
    if (model != o.model || display != o.display || crt != o.crt || tls_mode != o.tls_mode || tls_cert_sha256 != o.tls_cert_sha256 ||
        tls_min_version != o.tls_min_version)
        return false;
    for (size_t i = 0; i < kSessions; ++i) {
        const ProfileSession &a = sessions[i], &b = o.sessions[i];
        if (a.host != b.host || a.port != b.port || a.tls != b.tls || a.device_name != b.device_name ||
            a.terminal_type != b.terminal_type || a.cert_sha256 != b.cert_sha256)
            return false;
    }
    return true;
}

bool parse_profile(const std::string& text, const std::string& origin, Profile& out, std::string& error) {
    toml::table root;
    try {
        root = toml::parse(text, origin);
    } catch (const toml::parse_error& e) {
        std::ostringstream os;
        os << origin << ": " << e.description() << " (line " << e.source().begin.line << ")";
        error = os.str();
        return false;
    }
    Profile p;
    if (auto f = root["format"].value<int64_t>()) {
        if (*f > Profile::kFormat) {
            error = origin + ": written by a newer em34xx (profile format " + std::to_string(*f) + ")";
            return false;
        }
    }
    if (auto v = root["model"].value<std::string>()) p.model = *v;
    if (const toml::table* t = root["display"].as_table()) {
        if (auto v = (*t)["subtype"].value<std::string>()) {
            if (!parse_display_subtype(*v, p.display)) {
                error = origin + ": display.subtype must be \"green\", \"amber\", \"paperwhite\" or \"color\"";
                return false;
            }
        }
        if (auto v = (*t)["crt"].value<bool>()) p.crt = *v;
    }
    bool tls_default = false;   // format 1: one switch for every session
    if (const toml::table* t = root["tls"].as_table()) {
        if (auto v = (*t)["enabled"].value<bool>()) tls_default = *v;
        if (auto v = (*t)["verify"].value<std::string>()) {
            if (!parse_profile_tls_mode(*v, p.tls_mode)) {
                error = origin + ": tls.verify must be \"system\", \"none\" or \"fingerprint\"";
                return false;
            }
        }
        if (auto v = (*t)["cert_sha256"].value<std::string>()) p.tls_cert_sha256 = *v;
        if (auto v = (*t)["min_version"].value<std::string>()) p.tls_min_version = *v;
    }
    if (const toml::node* n = root.get("session")) {
        const toml::array* arr = n->as_array();
        if (!arr) {
            error = origin + ": session must be an array of tables ([[session]])";
            return false;
        }
        if (arr->size() > Profile::kSessions) {
            error = origin + ": at most 2 sessions";
            return false;
        }
        size_t i = 0;
        for (const toml::node& node : *arr) {
            const toml::table* t = node.as_table();
            if (!t) {
                error = origin + ": [[session]] entries must be tables";
                return false;
            }
            ProfileSession& s = p.sessions[i++];
            s.tls = tls_default;
            if (auto v = (*t)["host"].value<std::string>()) s.host = trim(*v);
            if (auto v = (*t)["tls"].value<bool>()) s.tls = *v;
            if (auto v = (*t)["port"].value<int64_t>()) {
                if (*v <= 0 || *v > 65535) {
                    error = origin + ": session port out of range";
                    return false;
                }
                s.port = uint16_t(*v);
            }
            if (auto v = (*t)["device_name"].value<std::string>()) s.device_name = *v;
            if (auto v = (*t)["terminal_type"].value<std::string>()) s.terminal_type = trim(*v);
            if (auto v = (*t)["cert_sha256"].value<std::string>()) s.cert_sha256 = *v;
        }
    }
    out = p;
    return true;
}

std::string profile_to_toml(const Profile& p) {
    toml::table display{{"subtype", display_subtype_name(p.display)}, {"crt", p.crt}};
    toml::table tls{{"verify", profile_tls_mode_name(p.tls_mode)}};
    if (!p.tls_cert_sha256.empty()) tls.insert("cert_sha256", p.tls_cert_sha256);
    if (!p.tls_min_version.empty()) tls.insert("min_version", p.tls_min_version);
    toml::array sessions;
    for (size_t i = 0; i < Profile::kSessions; ++i) {
        const ProfileSession& s = p.sessions[i];
        if (i > 0 && !s.used()) continue;
        toml::table t{{"host", s.host}, {"port", int64_t(s.port)}, {"tls", s.tls}};
        if (!s.device_name.empty()) t.insert("device_name", s.device_name);
        if (!s.terminal_type.empty()) t.insert("terminal_type", s.terminal_type);
        if (!s.cert_sha256.empty()) t.insert("cert_sha256", s.cert_sha256);
        sessions.push_back(std::move(t));
    }
    toml::table root{{"format", int64_t(Profile::kFormat)},
                     {"model", p.model},
                     {"display", display},
                     {"tls", tls},
                     {"session", sessions}};
    std::ostringstream os;
    os << "# em34xx connection profile (docs/frontend-win32.md). Passwords are never stored.\n\n" << root << "\n";
    return os.str();
}

bool load_profile(const std::string& path, Profile& out, std::string& error) {
    std::string text;
    if (!read_text_file(path, text, error)) return false;
    return parse_profile(text, path, out, error);
}

bool save_profile(const std::string& path, const Profile& p, std::string& error) {
    return write_text_file(path, profile_to_toml(p), error);
}

bool profile_connect_config(const Profile& p, const AppConfig& base, TlsVersion default_tls_min, AppConfig& out,
                            std::string& error) {
    error = p.validate();
    if (!error.empty()) return false;
    out = base;
    const ProfileSession& s1 = p.sessions[0];
    out.twinax.host = TwinaxHostKind::Tn5250;
    out.twinax.stations.clear();
    Tn5250Config& tn = out.tn5250;
    tn.host = s1.host;
    tn.port = s1.port;
    tn.device_name = s1.device_name;
    // [tn5250] is session 1; session 2 overrides what differs (StationConfig).
    tn.tls = s1.tls;
    tn.tls_verify = p.tls_mode == ProfileTlsMode::AcceptAll ? TlsVerify::None : TlsVerify::System;
    const std::string pin = p.tls_mode == ProfileTlsMode::Fingerprint ? p.tls_cert_sha256 : std::string();
    tn.tls_cert_sha256 = s1.tls ? pin : std::string();
    tn.tls_min_version = default_tls_min;
    if (!p.tls_min_version.empty()) parse_tls_version(p.tls_min_version, tn.tls_min_version);
    if (tn.tls_max_version != TlsVersion::Any && tn.tls_max_version < tn.tls_min_version)
        tn.tls_max_version = TlsVersion::Any;
    if (!tn.tls) tn.ca_file.clear();

    StationConfig st0{0, StationType::Display, "", s1.terminal_type};
    if (s1.tls && !s1.cert_sha256.empty()) st0.tls_cert_sha256 = s1.cert_sha256;
    out.twinax.stations.push_back(st0);
    const ProfileSession& s2 = p.sessions[1];
    if (s2.used()) {
        StationConfig st1{1, StationType::Display, s2.device_name, s2.terminal_type};
        if (s2.host != s1.host) st1.host = s2.host;
        if (s2.port != s1.port) st1.port = s2.port;
        st1.tls = s2.tls ? 1 : 0;
        if (s2.tls) {
            if (!s2.cert_sha256.empty()) st1.tls_cert_sha256 = s2.cert_sha256;
            else if (!s1.tls) st1.tls_cert_sha256 = pin;   // [tn5250] is plain: the profile's pin goes here
        }
        out.twinax.stations.push_back(st1);
    }
    error = out.tn5250.validate();
    if (error.empty()) error = out.twinax.validate();
    return error.empty();
}

void profile_trust_certificate(Profile& p, unsigned station, const Sha256Digest& sha256) {
    const std::string fp = format_sha256_fingerprint(sha256);
    const ProfileSession& s1 = p.sessions[0];
    if (station < Profile::kSessions && station > 0) {
        ProfileSession& s = p.sessions[station];
        if (s.host != s1.host || s.port != s1.port) {
            s.cert_sha256 = fp;
            return;
        }
    }
    p.tls_mode = ProfileTlsMode::Fingerprint;
    p.tls_cert_sha256 = fp;
    p.sessions[0].cert_sha256.clear();
}

bool terminal_type_is_colour(const std::string& terminal_type) {
    const std::string t = to_lower(trim(terminal_type));
    return t == "ibm-3179-2" || t == "ibm-5292-2" || t == "ibm-3477-fc" || t == "ibm-3487-hc";
}

std::string profile_display_warning(const Profile& p) {
    if (p.display == DisplaySubtype::Color) return {};
    for (size_t i = 0; i < Profile::kSessions; ++i) {
        const ProfileSession& s = p.sessions[i];
        if (!s.used() || !terminal_type_is_colour(s.terminal_type)) continue;
        return format("Session %zu announces the colour terminal type %s, but the CRT type is %s. The host will "
                      "send colour attributes, which a monochrome display shows as blinking, bright or "
                      "underlined text.",
                      i + 1, s.terminal_type.c_str(), display_subtype_name(p.display));
    }
    return {};
}

} // namespace em34xx
