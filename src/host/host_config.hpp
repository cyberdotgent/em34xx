// SPDX-License-Identifier: GPL-3.0-or-later
// Configuration surface for the host side of the emulated terminal.
//
// The emulator will eventually reach an IBM i host over TN5250 (RFC 1205 /
// RFC 2877). Only the configuration is defined so far: the session itself
// is a stub. Data reaches the firmware through the emulated Twinax link
// hardware, never through ROM patches.
#pragma once


#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace em34xx {

// How the TLS transport checks the server certificate (see docs/tn5250.md).
enum class TlsVerify {
    System,   // chain to the host's certificate store (+ ca_file) and hostname
    None,     // accept any certificate; its fingerprint is logged
};

const char* tls_verify_name(TlsVerify v);
// "system" (also "default", "on", "true") or "none" (also "insecure",
// "off", "false").
bool parse_tls_verify(const std::string& text, TlsVerify& out);

// Protocol version bounds for TLS. Any: no bound (only valid as a maximum).
enum class TlsVersion { Any = 0, Ssl3, Tls1_0, Tls1_1, Tls1_2, Tls1_3 };

// "ssl3", "1.0", "1.1", "1.2", "1.3" ("any" for Any).
const char* tls_version_name(TlsVersion v);
// Accepts those names, also with a "tls"/"tlsv" prefix ("TLSv1.2",
// "tls1.0"), and "sslv3"; "any" or "" give Any.
bool parse_tls_version(const std::string& text, TlsVersion& out);

struct Tn5250Config {
    std::string host;                    // empty = not configured
    uint16_t port = 23;
    std::string device_name;             // DEVNAME; empty lets the host choose
    std::string terminal_type;           // RFC 1205 type; empty = derive per station
                                         // (see station_terminal_type)
    bool tls = false;                    // TLS from the first byte (port 992 by convention)
    TlsVerify tls_verify = TlsVerify::System;
    // SHA-256 fingerprint of the server's leaf certificate. When set it
    // decides alone: a match is accepted even if the chain does not verify,
    // a mismatch is refused even if it does.
    std::string tls_cert_sha256;
    std::string ca_file;                 // extra trust anchors (PEM), on top of the system store
    // Protocol versions. 1.2 and newer by default; old IBM i / OS/400
    // hosts may need 1.0 or 1.1 (some only SSLv3).
    TlsVersion tls_min_version = TlsVersion::Tls1_2;
    TlsVersion tls_max_version = TlsVersion::Any;
    std::string codepage = "37";         // EBCDIC CCSID used for translation
    std::map<std::string, std::string> env;  // extra RFC 2877 NEW-ENVIRON variables

    bool configured() const { return !host.empty(); }
    // Returns an empty string when valid, otherwise a description.
    std::string validate() const;
    std::string describe() const;
};

// Which controller-side implementation drives the emulated Twinax line.
enum class TwinaxHostKind {
    None,     // nothing on the line: the terminal sees no controller
    Script,   // frames are injected from the monitor ("twinax send")
    Tn5250,   // bridge to a TN5250 session (not implemented yet)
    Wsc,      // the workstation controller with a local screen, no TN5250 session
};

const char* twinax_host_kind_name(TwinaxHostKind kind);
bool parse_twinax_host_kind(const std::string& text, TwinaxHostKind& out);

// What a station address of the terminal presents to the controller.
enum class StationType {
    Display,  // a display session (keyboard + screen)
    Printer,  // a printer session (the terminal's printer port)
};

const char* station_type_name(StationType type);

bool parse_station_type(const std::string& text, StationType& out);

// One station address the controller drives. A terminal can answer on
// several (the 3477 and 3486 on two, the 3489 on four), each one an
// independent 5250 station with its own TN5250 session.
struct StationConfig {
    unsigned address = 0;                // 0..6; 7 is the end-of-message marker
    StationType type = StationType::Display;
    std::string device_name;             // TN5250 DEVNAME; empty = host assigned
                                         // (the first display falls back to tn5250.device_name)
    std::string profile;                 // TN5250 terminal type; empty = default for the type
    // TLS protocol bounds for this station's session; empty = the
    // [tn5250] setting. Values as parse_tls_version().
    std::string tls_min_version{};   // (initialised: keeps 4-member brace
                                     // initialisers free of -Wmissing-field-initializers)
    std::string tls_max_version{};
    // A session to another system than [tn5250]: host and port (0 = the
    // [tn5250] port), and the SHA-256 of its TLS certificate when pinned
    // (empty = the [tn5250] setting). Display stations only.
    std::string host{};
    uint16_t port = 0;
    std::string tls_cert_sha256{};
    // TLS for this station's session: -1 = the [tn5250] setting, 0 = off
    // (plain telnet), 1 = on. Display stations only.
    int tls = -1;
};

// Terminal type for one station: its `profile`, else tn5250.terminal_type
// (--tn5250-type), else `unit_type`: the RFC 1205 type ("IBM-tttt-mm") of
// the emulated unit (ModelVariant::tn5250_type). The 5250 engine derives
// its Query Reply (screen sizes, colour) from the type, so it follows what
// the unit supports.
std::string station_terminal_type(const Tn5250Config& tn5250, const StationConfig& station,
                                  const std::string& unit_type);

// Apply a station's TLS overrides (tls, tls_min_version, tls_max_version,
// tls_cert_sha256) to the session configuration built from [tn5250].
// Unparsable values are left alone; TwinaxConfig::validate() reports them.
void apply_station_tls(Tn5250Config& cfg, const StationConfig& station);

// Parses the CLI shorthand ADDR[:TYPE[:DEVNAME]], e.g. "1:printer:PRT01".
bool parse_station_spec(const std::string& text, StationConfig& out, std::string& error);

struct TwinaxConfig {
    TwinaxHostKind host = TwinaxHostKind::None;
    // Legacy single-station form (--station-address, twinax.station_address):
    // used only while `stations` is empty.
    unsigned station_address = 0;
    std::vector<StationConfig> stations;

    // The configured stations, or the legacy single display station.
    std::vector<StationConfig> effective_stations() const;
    // max_sessions = the terminal model's limit (0 = do not check);
    // max_printers = how many of them may be printers (-1 = do not check).
    std::string validate(unsigned max_sessions = 0, int max_printers = -1) const;

    // A 5250 Twinax cable carries at most 7 stations: addresses 0..6, with
    // 7 reserved as the end-of-message marker.
    static constexpr unsigned kMaxStationsPerBus = 7;
};

} // namespace em34xx
