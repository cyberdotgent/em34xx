// SPDX-License-Identifier: GPL-3.0-or-later
// The boundary between the emulated terminal's Twinax hardware and whatever
// plays the controller (the primary station) on the other end of the cable.
//
//   firmware <-> [model-specific link device] <-> TwinaxHost <-> network/script
//
// The frame and the TwinaxHost contract live in machine/twinax_port.hpp (the
// emulator core owns the line); this file adds the controllers that do not
// need the TN5250 client: the scripted host and the configuration-only
// TN5250 placeholder, and the factory that picks one from the configuration.
#pragma once

#include "host/host_config.hpp"
#include "machine/twinax_port.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tn34xx {

// Frames are queued by the monitor and responses are recorded for
// inspection. This is the bring-up path for the link device.
class ScriptTwinaxHost : public TwinaxHost {
public:
    const char* kind() const override { return "script"; }
    void from_station(const TwinaxMessage& msg) override;
    bool to_station(TwinaxMessage& out) override;
    void no_response() override;
    std::string describe() const override;

    void queue(TwinaxMessage msg);
    std::vector<TwinaxMessage> take_received();

private:
    mutable std::mutex mutex_;
    std::deque<TwinaxMessage> outgoing_;
    std::vector<TwinaxMessage> received_;
    uint64_t sent_count_ = 0;
    uint64_t no_response_count_ = 0;
};

// Configuration-only stand-in for the TN5250 bridge, used when no bridge
// factory is linked in (see make_twinax_host). It never drives the line.
class Tn5250TwinaxHost : public TwinaxHost {
public:
    explicit Tn5250TwinaxHost(Tn5250Config cfg) : cfg_(std::move(cfg)) {}
    const char* kind() const override { return "tn5250"; }
    void from_station(const TwinaxMessage& msg) override;
    bool to_station(TwinaxMessage&) override { return false; }
    std::string describe() const override;

private:
    Tn5250Config cfg_;
};

// Builds the TN5250 bridge (wsc::make_tn5250_bridge, in the tn34xx_bridge
// library, which needs the TN5250 client). make_twinax_host() validates the
// configuration and then calls it; without one it falls back to the
// configuration-only Tn5250TwinaxHost placeholder.
using Tn5250HostFactory = std::function<std::unique_ptr<TwinaxHost>(const TwinaxConfig&, const Tn5250Config&,
                                                                    std::string& error)>;

std::unique_ptr<TwinaxHost> make_twinax_host(const TwinaxConfig& twinax, const Tn5250Config& tn5250,
                                             std::string& error, const Tn5250HostFactory& tn5250_factory = nullptr);

} // namespace tn34xx
