// SPDX-License-Identifier: GPL-3.0-or-later
// Connection profiles: what em34xxw saves as a `.em34xx` file (TOML) and
// connects with. A profile names the terminal model and its display, and up
// to two TN5250 display sessions (Twinax station addresses 0 and 1), each
// with or without TLS. It never holds a password. Format
// (docs/frontend-win32.md):
//
//   format = 2
//   model = "3477"                 terminal emulation type (runnable_models())
//
//   [display]
//   subtype = "green"              CRT type: green, amber, paperwhite, color
//   crt = true                     CRT scan-line rendering
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
//   terminal_type = ""             optional TN5250 terminal type (RFC 1205,
//                                  e.g. "IBM-3179-2") for a host that does
//                                  not accept the model's own; absent = the
//                                  type derived from the model and CRT type
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
    // TN5250 terminal type; empty = derived from the model and CRT type
    // (station_terminal_type). Last, so positional initialisers of the
    // members above keep their meaning.
    std::string terminal_type{};
    bool used() const { return !host.empty(); }
};

struct Profile {
    static constexpr int kFormat = 2;
    static constexpr size_t kSessions = 2;

    std::string model = "3477";
    DisplaySubtype display = DisplaySubtype::Green;
    bool crt = true;               // CRT scan-line rendering
    ProfileTlsMode tls_mode = ProfileTlsMode::System;
    std::string tls_cert_sha256;
    std::string tls_min_version;   // "" = application default; else as parse_tls_version
    std::array<ProfileSession, kSessions> sessions;

    // Whether any used session connects with TLS.
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

// Whether a host that is told `terminal_type` treats the display as a
// colour one (it then sends colour attributes, which a monochrome tube
// shows as blink, high intensity and column separators): the 3179, 5292 and
// 3477-FC / 3487-HC types (documented: RFC 1205 and the 3477/3487 model
// designations). Unknown types: false.
bool terminal_type_is_colour(const std::string& terminal_type);
// What a profile editor should warn about before connecting: a session
// whose explicit terminal type is a colour one while the CRT type is
// monochrome. Empty when there is nothing to say.
std::string profile_display_warning(const Profile& p);

} // namespace em34xx
