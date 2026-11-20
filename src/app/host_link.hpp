// The terminal's connection to a host: which controller is on the Twinax
// line (none, script, the TN5250 bridge) and the TN5250 sessions behind it.
// This is the programmatic API a GUI calls for connect / disconnect /
// status; the monitor's `connect` and `disconnect` commands are thin
// wrappers over it.
#pragma once

#include "config/config.hpp"
#include "host/twinax_link.hpp"

#include <string>
#include <vector>

namespace tn34xx {

class Emulator;

class HostLink {
public:
    // `base` is the start-up configuration (defaults for every setting a
    // connect request does not give); the limits are the model's.
    HostLink(Emulator& emu, AppConfig base, unsigned max_sessions, int max_printers, Tn5250HostFactory bridge);

    bool bridge_available() const { return static_cast<bool>(bridge_); }
    const AppConfig& base() const { return base_; }

    // Put the TN5250 bridge on the line with cfg's [twinax] and [tn5250]
    // settings, replacing the current controller (its sessions close).
    // On success `message` describes what happens next.
    bool connect(const AppConfig& cfg, std::string& message, std::string& error);
    // The same from monitor-style arguments: {"connect", "HOST[:PORT]",
    // options...}, see config/connect_args.hpp.
    bool connect(const std::vector<std::string>& args, std::string& message, std::string& error);

    // Take the TN5250 bridge off the line (no controller afterwards).
    bool disconnect(std::string& error);
    // Close the session behind one station address and keep it closed.
    bool disconnect_station(unsigned address, std::string& error);

    // The controller on the line: "none", "script", "tn5250", "wsc"...
    std::string controller_kind() const;
    // Multi-line description of the controller and its sessions.
    std::string status() const;

private:
    Emulator& emu_;
    AppConfig base_;
    unsigned max_sessions_;
    int max_printers_;
    Tn5250HostFactory bridge_;
};

} // namespace tn34xx
