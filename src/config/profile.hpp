// SPDX-License-Identifier: GPL-3.0-or-later
// Connection profiles: what em34xxw saves as a `.em34xx` file (TOML) and
// connects with. A profile names the terminal model and its display, and up
// to two TN5250 display sessions (Twinax station addresses 0 and 1), each
// with or without TLS. It never holds a password. Format
// (docs/frontend-win32.md):
//
//   format = 3
//   model = "3477-FG"              terminal type: a model's variant
//                                  (ModelVariant::id). A bare model name
//                                  ("3477") is its first variant
//
//   [display]
//   phosphor = "green"             tube of a monochrome variant (Phosphor::id);
//                                  absent = the variant's first
//   crt = true                     CRT scan-line rendering
//
// Formats 1 and 2 had `model = "3477"` and `display.subtype` (green, amber,
// paperwhite, color), which are read as the model's monochrome variant
// with that phosphor, or its colour variant (select_legacy_display).
//
//   [tls]                          for the sessions with tls = true
//   verify = "system"              "system" (verify normally), "none" (accept
//                                  any certificate) or "fingerprint" (accept
//                                  exactly the certificate below)
//   cert_sha256 = "AB:CD:..."      for "fingerprint"
//   min_version = "1.2"            optional; absent = the application default
//
//   [[session]]                    session 1 (station 0); host required
//   host = "pub400.com"
//   port = 992
//   tls = true
//   device_name = ""               optional DEVNAME
//
//   [[session]]                    session 2 (station 1); optional
//   host = ""                      empty = not used
//   port = 23
//   tls = false
//   cert_sha256 = ""               optional: the trusted certificate of this
//                                  session's host when it is another system
//
// Format 1 (em34xx 0.1) had one `tls.enabled` for both sessions and no
// [display]; it still loads (a session without `tls` takes tls.enabled).
#pragma once

#include "config/config.hpp"
#include "machine/display_subtype.hpp"
#include "util/fingerprint.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace em34xx {

enum class ProfileTlsMode {
    System,        // verify against the certificate store and the host name
    AcceptAll,     // accept any certificate (insecure)
    Fingerprint,   // accept exactly the certificate with cert_sha256
};

const char* profile_tls_mode_name(ProfileTlsMode m);
bool parse_profile_tls_mode(const std::string& text, ProfileTlsMode& out);

struct ProfileSession {
    std::string host;              // empty = session not used
    uint16_t port = 23;
    std::string device_name;
    std::string cert_sha256;       // per-session trusted certificate (another host)
    bool tls = false;              // TLS for this session (settings in Profile)
    bool used() const { return !host.empty(); }
};

struct Profile {
    static constexpr int kFormat = 3;
    static constexpr size_t kSessions = 2;

    std::string model = "3477";    // model name, without the variant
    std::string variant;           // ModelVariant::id; "" = the model's first
    std::string phosphor;          // Phosphor::id; "" = the variant's first
    bool crt = true;               // CRT scan-line rendering
    ProfileTlsMode tls_mode = ProfileTlsMode::System;
    std::string tls_cert_sha256;
    std::string tls_min_version;   // "" = application default; else as parse_tls_version
    std::array<ProfileSession, kSessions> sessions;

    // Whether any used session connects with TLS.
    // Fill in `variant` and `phosphor` with what "" stands for (and drop a
    // phosphor the variant does not have), so that two profiles for the same
    // unit compare equal. Nothing changes for a model this build does not
    // know.
    void normalize();
    bool any_tls() const;
    // Empty when the profile can be connected; else what is wrong.
    std::string validate() const;
    bool operator==(const Profile& o) const;
    bool operator!=(const Profile& o) const { return !(*this == o); }
};

bool parse_profile(const std::string& text, const std::string& origin, Profile& out, std::string& error);
std::string profile_to_toml(const Profile& p);
bool load_profile(const std::string& path, Profile& out, std::string& error);
bool save_profile(const std::string& path, const Profile& p, std::string& error);

// The configuration HostLink::connect() takes for a profile: `base` (the
// running application's settings) with the TN5250 bridge, one display
// station per used session, and the profile's TLS settings.
// default_tls_min: the application default when the profile sets none.
bool profile_connect_config(const Profile& p, const AppConfig& base, TlsVersion default_tls_min, AppConfig& out,
                            std::string& error);

// "Trust permanently" for the certificate a session (station address)
// presented: the profile trusts exactly that certificate from now on. For
// session 1, or a session 2 on the same host and port, that is the
// profile's fingerprint mode; for a session 2 on another system it is that
// session's own cert_sha256.
void profile_trust_certificate(Profile& p, unsigned station, const Sha256Digest& sha256);

} // namespace em34xx
