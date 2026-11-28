// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/profile.hpp"

#include "config/settings.hpp"
#include "models/model.hpp"
#include "util/fingerprint.hpp"
#include "util/strings.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
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

const ProfileSession& Profile::session(size_t i) const {
    static const ProfileSession unused;
    return i < sessions.size() ? sessions[i] : unused;
}

ProfileSession& Profile::session(size_t i) {
    if (i >= sessions.size()) sessions.resize(i + 1);
    return sessions[i];
}

size_t Profile::session_slots() const {
    const Model* m = find_model(model);
    size_t n = m ? m->info().sessions.size() : 1;
    for (size_t i = 0; i < sessions.size(); ++i)
        if (sessions[i].used()) n = std::max(n, i + 1);
    return std::max<size_t>(n, 1);
}

SessionKind Profile::session_kind(size_t i) const {
    const Model* m = find_model(model);
    return m && i < m->info().sessions.size() ? m->info().sessions[i] : SessionKind::Display;
}

bool Profile::any_tls() const {
    for (const ProfileSession& s : sessions)
        if (s.used() && s.tls) return true;
    return false;
}

std::string Profile::validate() const {
    if (trim(model).empty()) return "no terminal type";
    // A 3270 (coax) terminal has one session, and its device name is a
    // TN3270E LU name of at most 8 characters [documented: SNA LU names].
    const Model* m = find_model(model);
    const bool coax = m && m->info().line == LineKind::Coax;
    if (sessions.empty() || !sessions[0].used()) return "session 1 needs a host name";
    if (sessions.size() > kMaxSessions) return format("at most %zu sessions", kMaxSessions);
    if (m)
        for (size_t i = m->info().sessions.size(); i < sessions.size(); ++i)
            if (sessions[i].used()) {
                const size_t n = m->info().sessions.size();
                return format("the IBM %s has %s: clear session %zu", m->info().name.c_str(),
                              n == 1 ? "one session" : format("%zu sessions", n).c_str(), i + 1);
            }
    for (size_t i = 0; i < sessions.size(); ++i) {
        const ProfileSession& s = sessions[i];
        const std::string which = format("session %zu", i + 1);
        if (coax && s.device_name.size() > 8) return which + ": an LU name has at most 8 characters";
        if (!s.used()) {
            if (i == 0) return "session 1 needs a host name";
            continue;
        }
        if (s.host.find_first_of(" \t\r\n") != std::string::npos) return which + ": the host name contains blanks";
        if (s.port == 0) return which + ": the port must be 1..65535";
        if (s.device_name.size() > 10) return which + ": the device name is longer than 10 characters";
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

void Profile::normalize() {
    const Model* m = nullptr;
    const ModelVariant* v = nullptr;
    if (!find_model_variant(model, m, v) || !v) return;
    model = m->info().name;   // also for "3477-FG" in `model`
    if (!variant.empty())
        if (const ModelVariant* named = m->find_variant(variant)) v = named;
    variant = v->id;
    const Phosphor* p = v->phosphor(phosphor);
    phosphor = p ? p->id : std::string();
}

bool Profile::operator==(const Profile& other) const {
    Profile mine = *this, theirs = other;
    mine.normalize();
    theirs.normalize();
    const Profile& o = other;
    if (mine.model != theirs.model || mine.variant != theirs.variant || mine.phosphor != theirs.phosphor ||
        crt != o.crt || tls_mode != o.tls_mode || tls_cert_sha256 != o.tls_cert_sha256 ||
        tls_min_version != o.tls_min_version)
        return false;
    for (size_t i = 0; i < std::max(sessions.size(), o.sessions.size()); ++i) {
        const ProfileSession &a = session(i), &b = o.session(i);
        if (a.host != b.host || a.port != b.port || a.tls != b.tls || a.device_name != b.device_name || a.cert_sha256 != b.cert_sha256)
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
    const Model* model = nullptr;
    const ModelVariant* variant = nullptr;
    bool variant_named = false;
    if (auto v = root["model"].value<std::string>()) {
        p.model = *v;
        if (find_model_variant(*v, model, variant) && variant) {
            variant_named = to_lower(*v).find(to_lower(variant->id)) != std::string::npos;
            p.model = model->info().name;
            p.variant = variant->id;
        }
    }
    if (const toml::table* t = root["display"].as_table()) {
        if (auto v = (*t)["phosphor"].value<std::string>()) p.phosphor = *v;
        if (auto v = (*t)["subtype"].value<std::string>()) {
            // Formats 1 and 2.
            DisplaySubtype legacy;
            if (!parse_display_subtype(*v, legacy)) {
                error = origin + ": display.subtype must be \"green\", \"amber\", \"paperwhite\" or \"color\"";
                return false;
            }
            if (model && variant) {
                std::string legacy_variant, legacy_phosphor;
                select_legacy_display(*model, legacy, legacy_variant, legacy_phosphor);
                if (!variant_named) p.variant = legacy_variant;
                if (p.phosphor.empty()) p.phosphor = legacy_phosphor;
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
        if (arr->size() > Profile::kMaxSessions) {
            error = origin + format(": at most %zu sessions", Profile::kMaxSessions);
            return false;
        }
        size_t i = 0;
        for (const toml::node& node : *arr) {
            const toml::table* t = node.as_table();
            if (!t) {
                error = origin + ": [[session]] entries must be tables";
                return false;
            }
            ProfileSession& s = p.session(i++);
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
            if (auto v = (*t)["cert_sha256"].value<std::string>()) s.cert_sha256 = *v;
        }
    }
    p.normalize();
    out = p;
    return true;
}

std::string profile_to_toml(const Profile& profile) {
    Profile p = profile;
    p.normalize();   // the file names the variant and the phosphor
    toml::table display{{"crt", p.crt}};
    if (!p.phosphor.empty()) display.insert("phosphor", p.phosphor);
    toml::table tls{{"verify", profile_tls_mode_name(p.tls_mode)}};
    if (!p.tls_cert_sha256.empty()) tls.insert("cert_sha256", p.tls_cert_sha256);
    if (!p.tls_min_version.empty()) tls.insert("min_version", p.tls_min_version);
    // Session N is the Nth entry: an unused one before a used one stays.
    size_t count = 1;
    for (size_t i = 0; i < p.sessions.size(); ++i)
        if (p.sessions[i].used()) count = i + 1;
    toml::array sessions;
    for (size_t i = 0; i < count; ++i) {
        const ProfileSession& s = p.session(i);
        if (i > 0 && !s.used()) {
            sessions.push_back(toml::table{{"host", ""}});
            continue;
        }
        toml::table t{{"host", s.host}, {"port", int64_t(s.port)}, {"tls", s.tls}};
        if (!s.device_name.empty()) t.insert("device_name", s.device_name);
        if (!s.cert_sha256.empty()) t.insert("cert_sha256", s.cert_sha256);
        sessions.push_back(std::move(t));
    }
    toml::table root{{"format", int64_t(Profile::kFormat)},
                     {"model", p.variant.empty() ? p.model : p.variant},
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
    const ProfileSession& s1 = p.session(0);
    out.twinax.host = TwinaxHostKind::Tn5250;
    out.twinax.stations.clear();
    Tn5250Config& tn = out.tn5250;
    tn.host = s1.host;
    tn.port = s1.port;
    tn.device_name = to_upper(s1.device_name);   // DEVNAME / LU names are upper case
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

    // Session N is station address N - 1. What a session that can be
    // either turns out to be, the terminal says (StationType::Auto).
    auto station_type = [&](size_t i) {
        switch (p.session_kind(i)) {
        case SessionKind::Display: return StationType::Display;
        case SessionKind::Printer: return StationType::Printer;
        case SessionKind::DisplayOrPrinter: return StationType::Auto;
        }
        return StationType::Display;
    };
    StationConfig st0{0, station_type(0), "", ""};
    if (st0.type != StationType::Printer && s1.tls && !s1.cert_sha256.empty()) st0.tls_cert_sha256 = s1.cert_sha256;
    out.twinax.stations.push_back(st0);
    for (size_t i = 1; i < p.sessions.size(); ++i) {
        const ProfileSession& s = p.sessions[i];
        if (!s.used()) continue;
        StationConfig st{unsigned(i), station_type(i), to_upper(s.device_name), ""};
        if (st.type != StationType::Printer) {   // a printer session has no host session yet
            if (s.host != s1.host) st.host = s.host;
            if (s.port != s1.port) st.port = s.port;
            st.tls = s.tls ? 1 : 0;
            if (s.tls) {
                if (!s.cert_sha256.empty()) st.tls_cert_sha256 = s.cert_sha256;
                else if (!s1.tls) st.tls_cert_sha256 = pin;   // [tn5250] is plain: the profile's pin goes here
            }
        }
        out.twinax.stations.push_back(st);
    }
    error = out.tn5250.validate();
    if (error.empty()) error = out.twinax.validate();
    return error.empty();
}

void profile_trust_certificate(Profile& p, unsigned station, const Sha256Digest& sha256) {
    const std::string fp = format_sha256_fingerprint(sha256);
    const ProfileSession& s1 = p.session(0);
    if (station < p.sessions.size() && station > 0) {
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

} // namespace em34xx
