// SPDX-License-Identifier: GPL-3.0-or-later
#include "tn3270/client.hpp"

#include "tn5250/telnet.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <chrono>
#include <exception>

namespace em34xx {
namespace tn3270 {

const char* conn_state_name(ConnState s) {
    switch (s) {
    case ConnState::Idle: return "idle";
    case ConnState::Connecting: return "connecting";
    case ConnState::Negotiating: return "negotiating";
    case ConnState::Ready: return "ready";
    case ConnState::Closed: return "closed";
    case ConnState::Failed: return "failed";
    }
    return "?";
}

Client::Client(Tn5250Config cfg, TransportFactory factory) : cfg_(std::move(cfg)), factory_(std::move(factory)) {}

Client::~Client() { stop(); }

bool Client::start(std::string& error) {
    if (!cfg_.configured()) {
        error = "tn3270 host is not configured";
        return false;
    }
    error = cfg_.validate();
    if (!error.empty()) return false;
    if (thread_.joinable()) {
        error = "session already started";
        return false;
    }
    stop_ = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        outbound_.clear();
        outbound_bytes_ = 0;
        outbound_overflow_ = false;
    }
    state_ = ConnState::Connecting;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Client::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

std::string Client::last_error() const {
    std::lock_guard<std::mutex> g(mu_);
    return error_;
}

std::vector<std::string> Client::take_log() {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<std::string> out;
    if (log_dropped_) out.push_back(format("(%zu earlier log lines dropped)", log_dropped_));
    out.insert(out.end(), log_.begin(), log_.end());
    log_.clear();
    log_dropped_ = 0;
    return out;
}

void Client::log(const std::string& line) {
    TN_LOG("tn3270", Debug, "%s", line.c_str());
    std::lock_guard<std::mutex> g(mu_);
    log_locked(line);
}

void Client::log_locked(const std::string& line) {
    if (log_.size() >= kMaxLogLines) {
        log_.pop_front();
        ++log_dropped_;
    }
    log_.push_back(line);
}

std::string Client::lu_name() const {
    std::lock_guard<std::mutex> g(mu_);
    return lu_;
}

void Client::queue_outbound(const std::vector<uint8_t>& record) {
    std::vector<uint8_t> framed;
    if (tn3270e_) {
        // TN3270E header: 3270-DATA, no request or response flags, sequence
        // number 0 (no RESPONSES function) [RFC 2355 section 8].
        std::vector<uint8_t> r = {0, 0, 0, 0, 0};
        r.insert(r.end(), record.begin(), record.end());
        framed = tn5250::TelnetCodec::frame(r);
    } else {
        framed = tn5250::TelnetCodec::frame(record);
    }
    log(format("send record, %zu bytes, AID %02X", record.size(), record.empty() ? 0 : record[0]));
    std::lock_guard<std::mutex> g(mu_);
    if (outbound_overflow_ || outbound_bytes_ + framed.size() > kMaxQueuedBytes) {
        outbound_overflow_ = true;
        return;
    }
    outbound_bytes_ += framed.size();
    outbound_.push_back(std::move(framed));
}

void Client::pump() {
    std::deque<std::vector<uint8_t>> in;
    {
        std::lock_guard<std::mutex> g(mu_);
        in.swap(inbound_);
        inbound_bytes_ = 0;
    }
    std::vector<std::vector<uint8_t>> out;
    for (const auto& rec : in) engine_.process(rec, out);
    for (const auto& r : out) queue_outbound(r);
}

void Client::aid(uint8_t a) {
    std::vector<std::vector<uint8_t>> out;
    engine_.aid(a, out);
    for (const auto& r : out) queue_outbound(r);
}

void Client::fail(const std::string& why) {
    {
        std::lock_guard<std::mutex> g(mu_);
        error_ = why;
    }
    log("error: " + why);
    state_ = ConnState::Failed;
}

void Client::run() {
    try {
        run_session();
        return;
    } catch (const std::exception& e) {
        state_ = ConnState::Failed;
        try { fail(std::string("I/O thread: ") + e.what()); } catch (...) {}
    } catch (...) {
        state_ = ConnState::Failed;
        try { fail("I/O thread: unknown exception"); } catch (...) {}
    }
}

void Client::run_session() {
    std::string err;
    std::unique_ptr<tn5250::Transport> t = factory_ ? factory_(err) : tn5250::make_transport(cfg_, err);
    if (!t) {
        fail(err);
        return;
    }
    t->set_log([this](LogLevel level, const std::string& line) {
        if (log_enabled("tn3270", level)) log_write("tn3270", level, "%s", line.c_str());
        std::lock_guard<std::mutex> g(mu_);
        log_locked(line);
    });
    if (trust_) t->set_trust_handler(trust_);
    t->set_cancel(&stop_);
    log(format("connecting to %s:%u", cfg_.host.c_str(), cfg_.port));
    if (!t->connect(cfg_.host, cfg_.port, err)) {
        fail(err);
        return;
    }
    log("connected (" + t->describe() + ")");
    state_ = ConnState::Negotiating;

    tn5250::TelnetSettings settings;
    settings.terminal_type = cfg_.terminal_type.empty() ? kDefaultTerminalType : cfg_.terminal_type;
    // TN3270E when the server offers it, asking for the configured device
    // name as the LU (the same setting as a TN5250 session's DEVNAME).
    settings.tn3270e = true;
    settings.device_name = cfg_.device_name;
    tn5250::TelnetCodec codec(settings);
    std::vector<uint8_t> buf(8192), reply;
    std::vector<std::vector<uint8_t>> records;
    while (!stop_) {
        bool full;
        {
            std::lock_guard<std::mutex> g(mu_);
            full = inbound_bytes_ >= kMaxQueuedBytes;
        }
        int n = 0;
        if (full) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        else n = t->recv(buf.data(), buf.size(), 10);
        if (n < 0) {
            log("connection closed by host");
            state_ = ConnState::Closed;
            break;
        }
        if (n > 0) {
            reply.clear();
            records.clear();
            codec.receive(buf.data(), size_t(n), reply, records);
            for (auto& line : codec.take_log()) log(line);
            if (codec.failed()) {
                fail(codec.error());
                break;
            }
            if (!reply.empty() && !t->send(reply.data(), reply.size())) {
                fail("send failed");
                break;
            }
            if (codec.transparent() && state_ == ConnState::Negotiating) {
                tn3270e_ = codec.tn3270e_active();
                {
                    std::lock_guard<std::mutex> g(mu_);
                    lu_ = codec.tn3270e_lu();
                }
                state_ = ConnState::Ready;
                if (tn3270e_)
                    log("TN3270E mode, device type " + codec.tn3270e_device_type() +
                        (codec.tn3270e_lu().empty() ? std::string() : ", LU " + codec.tn3270e_lu()));
                else
                    log("3270 mode (BINARY + EOR both ways, terminal type sent)");
            }
            for (auto& rec : records) {
                if (tn3270e_) {
                    // Strip the TN3270E header; 3270-DATA and SSCP-LU-DATA
                    // (a USS screen before a BIND) carry a 3270 data stream.
                    if (rec.size() < 5) continue;
                    const uint8_t type = rec[0];
                    if (type != 0 && type != 7) {
                        log(format("recv TN3270E data type %02X, %zu bytes (ignored)", type, rec.size()));
                        continue;
                    }
                    rec.erase(rec.begin(), rec.begin() + 5);
                }
                log(format("recv record, %zu bytes, command %02X", rec.size(), rec.empty() ? 0 : rec[0]));
                std::lock_guard<std::mutex> g(mu_);
                inbound_bytes_ += rec.size();
                inbound_.push_back(std::move(rec));
            }
        }
        std::deque<std::vector<uint8_t>> out;
        bool overflow;
        {
            std::lock_guard<std::mutex> g(mu_);
            out.swap(outbound_);
            outbound_bytes_ = 0;
            overflow = outbound_overflow_;
        }
        if (overflow) {
            fail(format("more than %zu bytes of replies queued", kMaxQueuedBytes));
            break;
        }
        bool ok = true;
        for (const auto& frame : out)
            if (!t->send(frame.data(), frame.size())) {
                ok = false;
                break;
            }
        if (!ok) {
            fail("send failed");
            break;
        }
    }
    t->close();
    if (state_ == ConnState::Ready || state_ == ConnState::Negotiating || state_ == ConnState::Connecting)
        state_ = ConnState::Closed;
}

} // namespace tn3270
} // namespace em34xx
