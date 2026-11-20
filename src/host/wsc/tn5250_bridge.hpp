// The TN5250 bridge: the virtual workstation controller (LinkScheduler and
// one Station per configured address) with a TN5250 session behind each
// display station. This is the TwinaxHost behind `--twinax-host tn5250`.
//
//   IBM i <-> tn5250::Client (I/O thread) <-> engine (pump) <-> DisplayStationPort
//         <-> Station <-> LinkScheduler <-> TwinaxHost <-> terminal link device
//
// Threading. Every TwinaxHost call arrives on the emulation thread. The
// client's socket and telnet negotiation run on the client's own I/O
// thread; records cross on the client's queues and are applied to the
// engine only in pump(), which advance() calls. The controller and the
// engine therefore run on the emulation thread only, in emulated time.
//
// Session lifecycle (per display station; docs/twinax-bridge.md):
//   - The TN5250 session starts when the station first reaches Online
//     (identification done), so the host never sees a device that is not
//     there.
//   - Device lost (no response, power cycle, reset): the session stays
//     connected and the engine is told the device is gone. If the device
//     returns, the engine marks the whole screen dirty and the controller
//     repaints it. If it stays away for `lost_disconnect_ns` of emulated
//     time, the session is closed; the next Online starts a new one.
//   - Host ends the session (closed or failed connection): while the device
//     is online, the bridge reconnects with a fresh session after a back-off
//     on the wall clock (the host is a real network peer), doubling from
//     `reconnect_min_ms` to `reconnect_max_ms`. The new session's screen
//     replaces the old one through the normal diff.
//
// Printer stations are discovered, identified and polled, and get a
// placeholder printer session (NullPrinterSession) that accepts nothing and
// logs once that printer sessions are not implemented. A real
// tn5250::PrinterSession plugs in through PrinterStationPort.
#pragma once

#include "host/host_config.hpp"
#include "host/twinax_link.hpp"
#include "host/wsc/link_scheduler.hpp"
#include "host/wsc/session_port.hpp"
#include "host/wsc/tn5250_adapter.hpp"
#include "tn5250/client.hpp"
#include "tn5250/session_api.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tn34xx {
namespace wsc {

struct BridgeOptions {
    std::string model = "3477";                     // for the derived terminal type
    DisplaySubtype display = DisplaySubtype::Color;
    WscTiming timing;
    uint64_t lost_disconnect_ns = 600'000'000'000;  // 10 min of emulated time; 0 = never
    unsigned reconnect_min_ms = 2000;               // wall clock
    unsigned reconnect_max_ms = 60000;
    // Tests replace the network: one transport per session start.
    std::function<std::unique_ptr<tn5250::Transport>(const StationConfig&, std::string& error)> transport;
};

// The Tn5250Config one display station's session uses: the shared settings
// (host, port, TLS, code page, environment) with the station's own device
// name (the first display falls back to tn5250.device_name) and terminal
// type (station_terminal_type).
Tn5250Config station_session_config(const Tn5250Config& tn5250, const StationConfig& station, bool first_display,
                                    const std::string& model, DisplaySubtype display);

// A display station's DisplaySessionPort: owns the tn5250::Client (replaced
// on reconnect) and forwards to it through a Tn5250DisplayAdapter.
class DisplayStationPort : public DisplaySessionPort {
public:
    DisplayStationPort(StationConfig station, Tn5250Config cfg, const BridgeOptions& opt);
    ~DisplayStationPort() override;

    // DisplaySessionPort.
    void pump() override;
    void device_ready(const StationIdentity& id) override;
    void device_lost() override;
    uint64_t screen_generation() const override;
    void screen_snapshot(ScreenState& out) const override;
    bool take_alarm() override;
    void operator_action(const OperatorAction& action) override;

    // Lifecycle, driven by Tn5250Bridge::advance() in emulated time.
    void tick(uint64_t now_ns);

    const StationConfig& station() const { return station_; }
    const Tn5250Config& config() const { return cfg_; }
    tn5250::Client* client() { return client_.get(); }
    bool online() const { return online_; }
    unsigned sessions_started() const { return starts_; }
    std::string describe() const;

private:
    void start_session();
    void end_session(const char* why);

    StationConfig station_;
    Tn5250Config cfg_;
    const BridgeOptions& opt_;
    std::unique_ptr<tn5250::Client> client_;
    std::unique_ptr<Tn5250DisplayAdapter> adapter_;
    uint64_t generation_base_ = 0;    // keeps the generation monotonic across sessions
    bool online_ = false;
    uint64_t now_ = 0;
    uint64_t lost_at_ = 0;
    bool lost_pending_ = false;
    unsigned starts_ = 0;
    unsigned backoff_ms_ = 0;
    std::chrono::steady_clock::time_point retry_at_{};
    tn5250::ConnState last_state_ = tn5250::ConnState::Idle;
};

// Placeholder printer session: accepts nothing, logs once.
// TODO(printer): replace with a TN5250 printer session (RFC 4777 sections
// 8-10, SCS data) behind the same tn5250::PrinterSession interface.
class NullPrinterSession : public tn5250::PrinterSession {
public:
    explicit NullPrinterSession(StationConfig station) : station_(std::move(station)) {}
    void pump() override {}
    bool take_scs(std::vector<uint8_t>&) override { return false; }
    void acknowledge() override {}
    void report_status(uint16_t) override {}
    void set_device_ready(bool ready) override;

private:
    StationConfig station_;
    bool logged_ = false;
};

// Connects a tn5250::PrinterSession to a printer station's PrinterSessionPort.
class PrinterStationPort : public PrinterSessionPort {
public:
    explicit PrinterStationPort(std::unique_ptr<tn5250::PrinterSession> session) : session_(std::move(session)) {}
    void device_ready(const StationIdentity& id) override;
    void device_lost() override;
    bool next_print_data(std::vector<uint8_t>& scs) override;
    void printer_status(uint8_t byte2) override;

    void pump() { session_->pump(); }
    bool ready() const { return ready_; }
    uint8_t last_status() const { return status_; }

private:
    std::unique_ptr<tn5250::PrinterSession> session_;
    bool ready_ = false;
    uint8_t status_ = 0;
};

class Tn5250Bridge : public TwinaxHost {
public:
    Tn5250Bridge(const TwinaxConfig& twinax, const Tn5250Config& tn5250, BridgeOptions opt = BridgeOptions());
    ~Tn5250Bridge() override;

    const char* kind() const override { return "tn5250"; }
    void from_station(const TwinaxMessage& msg) override { sched_.from_station(msg); }
    bool to_station(TwinaxMessage& out) override { return sched_.to_station(out); }
    void no_response() override { sched_.no_response(); }
    void advance(uint64_t emu_ns) override;
    std::string describe() const override;

    LinkScheduler& scheduler() { return sched_; }
    DisplayStationPort* display(uint8_t address);
    PrinterStationPort* printer(uint8_t address);

private:
    BridgeOptions opt_;
    Tn5250Config tn5250_;
    LinkScheduler sched_;
    std::vector<std::unique_ptr<DisplayStationPort>> displays_;
    std::vector<std::pair<uint8_t, std::unique_ptr<PrinterStationPort>>> printers_;
};

// Validates the configuration and builds the bridge, or returns nullptr
// with `error` set.
std::unique_ptr<TwinaxHost> make_tn5250_bridge(const TwinaxConfig& twinax, const Tn5250Config& tn5250,
                                               BridgeOptions opt, std::string& error);

} // namespace wsc
} // namespace tn34xx
