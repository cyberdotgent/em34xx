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

namespace tn34xx {

struct Tn5250Config {
    std::string host;                    // empty = not configured
    uint16_t port = 23;
    std::string device_name;             // DEVNAME; empty lets the host choose
    std::string terminal_type = "IBM-3477-FC";
    bool tls = false;
    bool tls_verify = true;
    std::string ca_file;
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
};

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
    // max_sessions = the terminal model's limit (0 = do not check).
    std::string validate(unsigned max_sessions = 0) const;
};

} // namespace tn34xx
