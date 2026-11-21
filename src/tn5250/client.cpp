// SPDX-License-Identifier: GPL-3.0-or-later
#include "tn5250/client.hpp"

#include "tn5250/codepage.hpp"
#include "tn5250/record.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

namespace em34xx {
namespace tn5250 {

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

namespace {
// The caller normally resolves the type per station (station_terminal_type);
// an empty one here means a colour 3477.
std::string effective_type(const Tn5250Config& cfg) {
    return cfg.terminal_type.empty() ? derived_terminal_type("3477", DisplaySubtype::Color) : cfg.terminal_type;
}
} // namespace

TelnetSettings telnet_settings_for(const Tn5250Config& cfg) {
    TelnetSettings s;
    s.terminal_type = effective_type(cfg);
    auto add = [&](const std::string& name, const std::string& value, bool user) {
        s.environment.push_back(EnvVariable{name, std::vector<uint8_t>(value.begin(), value.end()), user});
    };
    auto env = cfg.env;
    auto take = [&](const char* name, std::string fallback) {
        auto it = env.find(name);
        if (it == env.end()) return fallback;
        std::string v = it->second;
        env.erase(it);
        return v;
    };
    if (!cfg.device_name.empty()) add("DEVNAME", cfg.device_name, true);
    env.erase("DEVNAME");
    const CodePage* cp = CodePage::find(cfg.codepage);
    const std::string kbd = take("KBDTYPE", cp ? cp->kbdtype() : "");
    const std::string codepage = take("CODEPAGE", cfg.codepage);
    const std::string charset = take("CHARSET", cp ? cp->charset() : "");
    // RFC 4777 section 4: CODEPAGE and CHARSET only mean something together
    // with KBDTYPE.
    if (!kbd.empty()) {
        add("KBDTYPE", kbd, true);
        if (!codepage.empty()) add("CODEPAGE", codepage, true);
        if (!charset.empty()) add("CHARSET", charset, true);
    }
    for (const auto& kv : env) {
        const std::string& n = kv.first;
        const bool standard = n == "USER" || n == "JOB" || n == "ACCT" || n == "PRINTER" ||
                              n == "SYSTEMTYPE" || n == "DISPLAY";
        add(n, kv.second, !standard);
    }
    return s;
}

Client::Client(Tn5250Config cfg, TransportFactory factory)
    : cfg_(std::move(cfg)), factory_(std::move(factory)),
      engine_(EngineOptions::for_terminal_type(effective_type(cfg_))) {
    engine_.set_record_sink([this](const Record& r) { queue_outbound(r); });
}

Client::~Client() { stop(); }

void Client::set_record_sink(RecordSink sink) {
    if (sink) engine_.set_record_sink(std::move(sink));
    else engine_.set_record_sink([this](const Record& r) { queue_outbound(r); });
}

bool Client::start(std::string& error) {
    if (!cfg_.configured()) { error = "tn5250 host is not configured"; return false; }
    error = cfg_.validate();
    if (!error.empty()) return false;
    if (thread_.joinable()) { error = "session already started"; return false; }
    stop_ = false;
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
    out.swap(log_);
    return out;
}

void Client::log(const std::string& line) {
    TN_LOG("tn5250", Debug, "%s", line.c_str());
    std::lock_guard<std::mutex> g(mu_);
    log_.push_back(line);
}

void Client::queue_outbound(const Record& rec) {
    const auto framed = TelnetCodec::frame(encode_record(rec));
    log(format("send record %s flags %s, %zu data bytes", opcode_name(rec.opcode), flags_text(rec.flags).c_str(),
               rec.data.size()));
    std::lock_guard<std::mutex> g(mu_);
    outbound_.push_back(framed);
}

void Client::pump() {
    std::deque<Record> in;
    {
        std::lock_guard<std::mutex> g(mu_);
        in.swap(inbound_);
    }
    engine_.set_system_available(state_.load() == ConnState::Ready);
    for (const Record& r : in) engine_.receive(r);
}

void Client::run() {
    std::string err;
    std::unique_ptr<Transport> t = factory_ ? factory_(err) : make_transport(cfg_, err);
    auto fail = [&](const std::string& why) {
        {
            std::lock_guard<std::mutex> g(mu_);
            error_ = why;
        }
        log("error: " + why);
        state_ = ConnState::Failed;
    };
    if (!t) { fail(err); return; }
    // Transport diagnostics (TLS certificate details) go to the session log,
    // and to the tn5250 log category at their own level, so that a
    // certificate warning shows without --log tn5250=debug.
    t->set_log([this](LogLevel level, const std::string& line) {
        if (log_enabled("tn5250", level)) log_write("tn5250", level, "%s", line.c_str());
        std::lock_guard<std::mutex> g(mu_);
        log_.push_back(line);
    });
    if (trust_) t->set_trust_handler(trust_);
    log(format("connecting to %s:%u", cfg_.host.c_str(), cfg_.port));
    if (!t->connect(cfg_.host, cfg_.port, err)) { fail(err); return; }
    log("connected (" + t->describe() + ")");
    state_ = ConnState::Negotiating;

    TelnetCodec codec(telnet_settings_for(cfg_));
    std::vector<uint8_t> buf(8192), reply;
    std::vector<std::vector<uint8_t>> records;
    while (!stop_) {
        const int n = t->recv(buf.data(), buf.size(), 10);
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
            if (!reply.empty() && !t->send(reply.data(), reply.size())) {
                fail("send failed");
                break;
            }
            if (codec.transparent() && state_ == ConnState::Negotiating) {
                state_ = ConnState::Ready;
                log("5250 transparent mode (BINARY + EOR both ways, terminal type sent)");
            }
            for (const auto& raw : records) {
                Record rec;
                std::string why, warn;
                if (!decode_record(raw, rec, why, &warn)) {
                    log("bad record: " + why);
                    continue;
                }
                if (!warn.empty()) log("record warning: " + warn);
                log(format("recv record %s flags %s, %zu data bytes", opcode_name(rec.opcode),
                           flags_text(rec.flags).c_str(), rec.data.size()));
                std::lock_guard<std::mutex> g(mu_);
                inbound_.push_back(std::move(rec));
            }
        }
        std::deque<std::vector<uint8_t>> out;
        {
            std::lock_guard<std::mutex> g(mu_);
            out.swap(outbound_);
        }
        bool ok = true;
        for (const auto& frame : out)
            if (!t->send(frame.data(), frame.size())) { ok = false; break; }
        if (!ok) { fail("send failed"); break; }
    }
    t->close();
    if (state_ == ConnState::Ready || state_ == ConnState::Negotiating || state_ == ConnState::Connecting)
        state_ = ConnState::Closed;
}

} // namespace tn5250
} // namespace em34xx
