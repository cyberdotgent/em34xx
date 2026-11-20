#include "host/wsc/tn5250_bridge.hpp"

#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>

namespace tn34xx {
namespace wsc {

Tn5250Config station_session_config(const Tn5250Config& tn5250, const StationConfig& station, bool first_display,
                                    const std::string& model, DisplaySubtype display) {
    Tn5250Config c = tn5250;   // host, port, TLS settings, code page, environment
    c.device_name = !station.device_name.empty() ? station.device_name
                    : first_display              ? tn5250.device_name
                                                 : std::string();
    c.terminal_type = station_terminal_type(tn5250, station, model, display);
    return c;
}

// ---------------------------------------------------------------------------
// DisplayStationPort

DisplayStationPort::DisplayStationPort(StationConfig station, Tn5250Config cfg, const BridgeOptions& opt)
    : station_(std::move(station)), cfg_(std::move(cfg)), opt_(opt) {
    // A session object exists from the start (its engine is the blank
    // shadow the controller paints before the host connects); it is
    // started only when the device is online.
    tn5250::Client::TransportFactory factory;
    if (opt_.transport) {
        const StationConfig st = station_;
        auto make = opt_.transport;
        factory = [make, st](std::string& error) { return make(st, error); };
    }
    client_ = std::make_unique<tn5250::Client>(cfg_, std::move(factory));
    adapter_ = std::make_unique<Tn5250DisplayAdapter>(*client_);
}

DisplayStationPort::~DisplayStationPort() {
    if (client_) client_->stop();
}

void DisplayStationPort::pump() {
    if (client_) client_->pump();
}

void DisplayStationPort::device_ready(const StationIdentity& id) {
    online_ = true;
    lost_pending_ = false;
    adapter_->device_ready(id);
    client_->set_device_ready(true);
    if (client_->state() == tn5250::ConnState::Idle) start_session();
}

void DisplayStationPort::device_lost() {
    online_ = false;
    adapter_->device_lost();
    client_->set_device_ready(false);
    lost_pending_ = true;
    lost_at_ = now_;
}

uint64_t DisplayStationPort::screen_generation() const { return generation_base_ + client_->generation(); }

void DisplayStationPort::screen_snapshot(ScreenState& out) const { adapter_->screen_snapshot(out); }

bool DisplayStationPort::take_alarm() { return adapter_->take_alarm(); }

void DisplayStationPort::operator_action(const OperatorAction& action) { adapter_->operator_action(action); }

void DisplayStationPort::start_session() {
    std::string error;
    if (client_->state() != tn5250::ConnState::Idle) {
        // A used client cannot be restarted: replace it. The generation
        // base keeps screen_generation() moving forward so the controller
        // notices the new (blank) shadow.
        generation_base_ += client_->generation() + 1;
        client_->stop();
        tn5250::Client::TransportFactory factory;
        if (opt_.transport) {
            const StationConfig st = station_;
            auto make = opt_.transport;
            factory = [make, st](std::string& err) { return make(st, err); };
        }
        client_ = std::make_unique<tn5250::Client>(cfg_, std::move(factory));
        adapter_ = std::make_unique<Tn5250DisplayAdapter>(*client_);
        client_->set_device_ready(online_);
    }
    ++starts_;
    last_state_ = tn5250::ConnState::Connecting;
    LOG_INFO("tn5250", "station %u: starting session to %s:%u%s, terminal type %s, device %s", station_.address,
             cfg_.host.c_str(), cfg_.port, cfg_.tls ? " (TLS)" : "", cfg_.terminal_type.c_str(),
             cfg_.device_name.empty() ? "(host assigned)" : cfg_.device_name.c_str());
    if (!client_->start(error)) {
        LOG_ERROR("tn5250", "station %u: %s", station_.address, error.c_str());
        last_state_ = tn5250::ConnState::Failed;
    }
}

void DisplayStationPort::end_session(const char* why) {
    LOG_INFO("tn5250", "station %u: closing session (%s)", station_.address, why);
    client_->stop();
}

void DisplayStationPort::tick(uint64_t now_ns) {
    now_ = now_ns;
    using tn5250::ConnState;
    const ConnState st = client_->state();
    if (st != last_state_) {
        if (st == ConnState::Ready) {
            LOG_INFO("tn5250", "station %u: session ready", station_.address);
            backoff_ms_ = 0;
        } else if (st == ConnState::Closed || st == ConnState::Failed) {
            const std::string err = client_->last_error();
            LOG_WARN("tn5250", "station %u: session %s%s%s", station_.address, tn5250::conn_state_name(st),
                     err.empty() ? "" : ": ", err.c_str());
            backoff_ms_ = backoff_ms_ ? std::min(backoff_ms_ * 2, opt_.reconnect_max_ms) : opt_.reconnect_min_ms;
            retry_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoff_ms_);
        }
        last_state_ = st;
    }

    // Device gone too long: let the host free the job.
    if (!online_ && lost_pending_ && opt_.lost_disconnect_ns && now_ns - lost_at_ >= opt_.lost_disconnect_ns) {
        lost_pending_ = false;
        if (st == ConnState::Ready || st == ConnState::Negotiating || st == ConnState::Connecting) {
            end_session("device absent");
            last_state_ = client_->state();
            backoff_ms_ = 0;
        }
    }

    // Device online but no live session: (re)connect.
    if (online_) {
        if (st == ConnState::Idle) {
            start_session();
        } else if ((st == ConnState::Closed || st == ConnState::Failed) &&
                   (backoff_ms_ == 0 || std::chrono::steady_clock::now() >= retry_at_)) {
            start_session();
        }
    }
}

std::string DisplayStationPort::describe() const {
    std::string out = format("station %u display: session %s to %s:%u%s, type %s, device %s, %u start(s)",
                             station_.address, tn5250::conn_state_name(client_->state()), cfg_.host.c_str(),
                             cfg_.port, cfg_.tls ? " TLS" : "", cfg_.terminal_type.c_str(),
                             cfg_.device_name.empty() ? "(host assigned)" : cfg_.device_name.c_str(), starts_);
    const std::string err = client_->last_error();
    if (!err.empty()) out += ", last error: " + err;
    return out + "\n";
}

// ---------------------------------------------------------------------------
// Printer stub

void NullPrinterSession::set_device_ready(bool ready) {
    if (!ready || logged_) return;
    logged_ = true;
    LOG_WARN("tn5250", "station %u: printer found, but TN5250 printer sessions are not implemented yet; "
                       "it is polled for status and receives no data",
             station_.address);
}

void PrinterStationPort::device_ready(const StationIdentity& id) {
    ready_ = true;
    LOG_INFO("wsc", "station %u: printer type %s", id.address, printer_type_name(id.device_id));
    session_->set_device_ready(true);
}

void PrinterStationPort::device_lost() {
    ready_ = false;
    session_->set_device_ready(false);
}

bool PrinterStationPort::next_print_data(std::vector<uint8_t>& scs) {
    if (!session_->take_scs(scs)) return false;
    session_->acknowledge();   // TODO(printer): acknowledge on the printer's print-complete status
    return true;
}

void PrinterStationPort::printer_status(uint8_t byte2) {
    status_ = byte2;
    session_->report_status(byte2);
}

// ---------------------------------------------------------------------------
// Tn5250Bridge

Tn5250Bridge::Tn5250Bridge(const TwinaxConfig& twinax, const Tn5250Config& tn5250, BridgeOptions opt)
    : opt_(std::move(opt)), tn5250_(tn5250), sched_(twinax.effective_stations(), opt_.timing) {
    bool first_display = true;
    for (const StationConfig& sc : twinax.effective_stations()) {
        Station* s = sched_.station(uint8_t(sc.address));
        if (sc.type == StationType::Display) {
            auto port = std::make_unique<DisplayStationPort>(
                sc, station_session_config(tn5250, sc, first_display, opt_.model, opt_.display), opt_);
            first_display = false;
            s->attach_display(port.get());
            displays_.push_back(std::move(port));
        } else {
            auto port = std::make_unique<PrinterStationPort>(std::make_unique<NullPrinterSession>(sc));
            s->attach_printer(port.get());
            printers_.emplace_back(uint8_t(sc.address), std::move(port));
        }
    }
}

Tn5250Bridge::~Tn5250Bridge() {
    // Stations hold raw pointers to the ports; detach before the ports go.
    for (auto& d : displays_) sched_.station(uint8_t(d->station().address))->attach_display(nullptr);
    for (auto& p : printers_) sched_.station(p.first)->attach_printer(nullptr);
}

void Tn5250Bridge::advance(uint64_t emu_ns) {
    sched_.advance(emu_ns);   // pumps every display session
    for (auto& d : displays_) d->tick(sched_.now());
    for (auto& p : printers_) p.second->pump();
}

DisplayStationPort* Tn5250Bridge::display(uint8_t address) {
    for (auto& d : displays_)
        if (d->station().address == address) return d.get();
    return nullptr;
}

PrinterStationPort* Tn5250Bridge::printer(uint8_t address) {
    for (auto& p : printers_)
        if (p.first == address) return p.second.get();
    return nullptr;
}

std::string Tn5250Bridge::describe() const {
    std::string out = "twinax: TN5250 bridge\n";
    out += sched_.describe();
    for (const auto& d : displays_) out += "  " + d->describe();
    for (const auto& p : printers_)
        out += format("  station %u printer: session not implemented (placeholder), %s, status %02X\n", p.first,
                      p.second->ready() ? "online" : "not online", p.second->last_status());
    return out;
}

std::unique_ptr<TwinaxHost> make_tn5250_bridge(const TwinaxConfig& twinax, const Tn5250Config& tn5250,
                                               BridgeOptions opt, std::string& error) {
    error = twinax.validate();
    if (!error.empty()) return nullptr;
    error = tn5250.validate();
    if (!error.empty()) return nullptr;
    if (!tn5250.configured()) {
        error = "twinax host 'tn5250' needs a tn5250 host name";
        return nullptr;
    }
    return std::make_unique<Tn5250Bridge>(twinax, tn5250, std::move(opt));
}

} // namespace wsc
} // namespace tn34xx
