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

struct TwinaxConfig {
    TwinaxHostKind host = TwinaxHostKind::None;
    unsigned station_address = 0;        // 0..6; 7 is the end-of-message marker
    std::string validate() const;
};

} // namespace tn34xx
