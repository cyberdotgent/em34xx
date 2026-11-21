// SPDX-License-Identifier: GPL-3.0-or-later
// The terminal's connection to a host: which controller is on the Twinax
// line (none, script, the TN5250 bridge) and the TN5250 sessions behind it.
// This is the programmatic API a GUI calls for connect / disconnect /
// status; the monitor's `connect` and `disconnect` commands are thin
// wrappers over it.
#pragma once

#include "config/config.hpp"
#include "host/twinax_link.hpp"
#include "tn5250/tls_trust.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace em34xx {

class Emulator;

// The front end's TLS trust handler, shared by HostLink (which sets it) and
// the bridge factory (which hands it to each new bridge's sessions).
// Each bridge gets its own wrapper that remembers the certificates the user
// accepted, so the bridge's automatic reconnects do not ask again; a new
// connect() asks again (unless the profile now pins the certificate).
class TlsTrustBroker {
public:
    void set_handler(tn5250::TlsTrustHandler fn);
    bool has_handler() const;
    // The handler for one new bridge, or an empty function when none is set
    // (the configuration then decides alone).
    tn5250::TlsTrustHandler for_new_bridge();

private:
    mutable std::mutex mu_;
    tn5250::TlsTrustHandler handler_;
};

class HostLink {
public:
    // `base` is the start-up configuration (defaults for every setting a
    // connect request does not give); the limits are the model's.
    // `trust`: where set_tls_trust_handler() stores the handler; the bridge
    // factory reads it from there (Application passes the same broker to
    // both). Null: a private one, which a factory cannot see.
    HostLink(Emulator& emu, AppConfig base, unsigned max_sessions, int max_printers, Tn5250HostFactory bridge,
             std::shared_ptr<TlsTrustBroker> trust = nullptr);

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

    // Ask `fn` when a TLS certificate is refused by the configuration
    // (tn5250/tls_trust.hpp): it runs on a session's I/O thread and may
    // block until the user decides. Applies to the next connect(); null
    // restores the configured behaviour. Any thread.
    void set_tls_trust_handler(tn5250::TlsTrustHandler fn) { trust_->set_handler(std::move(fn)); }

    // The controller on the line: "none", "script", "tn5250", "wsc"...
    std::string controller_kind() const;
    // Multi-line description of the controller and its sessions.
    std::string status() const;
    // The sessions behind the stations (empty without a TN5250 bridge).
    std::vector<TwinaxSessionStatus> sessions() const;

private:
    Emulator& emu_;
    AppConfig base_;
    unsigned max_sessions_;
    int max_printers_;
    Tn5250HostFactory bridge_;
    std::shared_ptr<TlsTrustBroker> trust_;
};

} // namespace em34xx
