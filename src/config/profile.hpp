// SPDX-License-Identifier: GPL-3.0-or-later
// Connection profiles: what tn34xxw saves as a `.tn34xx` file (TOML) and
// connects with. A profile names the terminal model and up to two TN5250
// display sessions (Twinax station addresses 0 and 1) with their TLS
// settings. It never holds a password. Format (docs/frontend-win32.md):
//
//   format = 1
//   model = "3477"                 terminal emulation type (runnable_models())
//
//   [tls]
//   enabled = true
//   verify = "system"              "system" (verify normally), "none" (accept
//                                  any certificate) or "fingerprint" (accept
//                                  exactly the certificate below)
//   cert_sha256 = "AB:CD:..."      for "fingerprint"
//   min_version = "1.2"            optional; absent = the application default
//
//   [[session]]                    session 1 (station 0); host required
//   host = "pub400.com"
//   port = 992
//   device_name = ""               optional DEVNAME
//
//   [[session]]                    session 2 (station 1); optional
//   host = ""                      empty = not used
//   port = 23
//   cert_sha256 = ""               optional: the trusted certificate of this
//                                  session's host when it is another system
#pragma once

#include "config/config.hpp"
#include "util/fingerprint.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace tn34xx {

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
    bool used() const { return !host.empty(); }
};

struct Profile {
    static constexpr int kFormat = 1;
    static constexpr size_t kSessions = 2;

    std::string model = "3477";
    bool tls = false;
    ProfileTlsMode tls_mode = ProfileTlsMode::System;
    std::string tls_cert_sha256;
    std::string tls_min_version;   // "" = application default; else as parse_tls_version
    std::array<ProfileSession, kSessions> sessions;

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

} // namespace tn34xx
