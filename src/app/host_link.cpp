#include "app/host_link.hpp"

#include "config/connect_args.hpp"
#include "machine/emulator.hpp"
#include "util/strings.hpp"

#include <set>

namespace tn34xx {

void TlsTrustBroker::set_handler(tn5250::TlsTrustHandler fn) {
    std::lock_guard<std::mutex> g(mu_);
    handler_ = std::move(fn);
}

bool TlsTrustBroker::has_handler() const {
    std::lock_guard<std::mutex> g(mu_);
    return static_cast<bool>(handler_);
}

tn5250::TlsTrustHandler TlsTrustBroker::for_new_bridge() {
    tn5250::TlsTrustHandler fn;
    {
        std::lock_guard<std::mutex> g(mu_);
        fn = handler_;
    }
    if (!fn) return nullptr;
    struct Accepted {
        std::mutex mu;
        std::set<std::string> fingerprints;   // host:port fingerprint
    };
    auto accepted = std::make_shared<Accepted>();
    return [fn, accepted](const tn5250::TlsTrustRequest& req) {
        const std::string key =
            format("%s:%u %s", req.host.c_str(), req.port, format_sha256_fingerprint(req.peer.sha256).c_str());
        {
            std::lock_guard<std::mutex> g(accepted->mu);
            if (accepted->fingerprints.count(key)) return tn5250::TlsTrustDecision::AcceptOnce;
        }
        const tn5250::TlsTrustDecision d = fn(req);
        if (d != tn5250::TlsTrustDecision::Reject) {
            std::lock_guard<std::mutex> g(accepted->mu);
            accepted->fingerprints.insert(key);
        }
        return d;
    };
}

HostLink::HostLink(Emulator& emu, AppConfig base, unsigned max_sessions, int max_printers, Tn5250HostFactory bridge,
                   std::shared_ptr<TlsTrustBroker> trust)
    : emu_(emu), base_(std::move(base)), max_sessions_(max_sessions), max_printers_(max_printers),
      bridge_(std::move(bridge)), trust_(trust ? std::move(trust) : std::make_shared<TlsTrustBroker>()) {}

bool HostLink::connect(const AppConfig& cfg, std::string& message, std::string& error) {
    if (!bridge_) {
        error = "connect: the TN5250 bridge is not available in this build";
        return false;
    }
    error = cfg.twinax.validate(max_sessions_, max_printers_);
    if (error.empty()) error = cfg.tn5250.validate();
    if (!error.empty()) {
        error = "connect: " + error;
        return false;
    }
    std::unique_ptr<TwinaxHost> host = bridge_(cfg.twinax, cfg.tn5250, error);
    if (!host) {
        error = "connect: " + error;
        return false;
    }
    // Swap under the machine lock; the link device looks the host up on
    // every use, so the next poll comes from the new controller. The old
    // one (and its sessions' threads) goes away outside the lock.
    std::unique_ptr<TwinaxHost> old = emu_.with_machine([&](Machine& m) { return m.swap_twinax_host(std::move(host)); });
    const bool replaced = old && std::string(old->kind()) == "tn5250";
    old.reset();
    std::string stations;
    for (const StationConfig& st : cfg.twinax.effective_stations())
        stations += format(" %u (%s)", st.address, station_type_name(st.type));
    message = format("%sconnecting to %s:%u%s, station(s)%s; the controller goes on the line when the session is up\n",
                     replaced ? "previous session closed; " : "", cfg.tn5250.host.c_str(), cfg.tn5250.port,
                     cfg.tn5250.tls ? " (TLS)" : "", stations.c_str());
    return true;
}

bool HostLink::connect(const std::vector<std::string>& args, std::string& message, std::string& error) {
    if (!bridge_) {
        error = "connect: the TN5250 bridge is not available in this build";
        return false;
    }
    AppConfig cfg;
    if (!parse_connect_args(args, base_, cfg, error)) return false;
    return connect(cfg, message, error);
}

bool HostLink::disconnect(std::string& error) {
    std::unique_ptr<TwinaxHost> old = emu_.with_machine([](Machine& m) {
        if (!m.twinax_host() || std::string(m.twinax_host()->kind()) != "tn5250") return std::unique_ptr<TwinaxHost>();
        return m.swap_twinax_host(std::make_unique<NullTwinaxHost>());
    });
    if (!old) {
        error = "disconnect: not connected";
        return false;
    }
    old.reset();
    return true;
}

bool HostLink::disconnect_station(unsigned address, std::string& error) {
    const bool ok = emu_.with_machine(
        [&](Machine& m) { return m.twinax_host() && m.twinax_host()->disconnect_station(address); });
    if (!ok) error = format("disconnect: no TN5250 session on station %u", address);
    return ok;
}

std::string HostLink::controller_kind() const {
    return emu_.with_machine([](Machine& m) { return std::string(m.twinax_host() ? m.twinax_host()->kind() : "none"); });
}

std::string HostLink::status() const {
    return emu_.with_machine([](Machine& m) { return m.twinax_host() ? m.twinax_host()->describe() : std::string(); });
}

} // namespace tn34xx

namespace tn34xx {

std::vector<TwinaxSessionStatus> HostLink::sessions() const {
    return emu_.with_machine([](Machine& m) {
        return m.twinax_host() ? m.twinax_host()->session_status() : std::vector<TwinaxSessionStatus>();
    });
}

} // namespace tn34xx
