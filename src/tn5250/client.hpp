// A TN5250 display session driven by Tn5250Config: transport + telnet
// negotiation on an I/O thread, the 5250 engine on the caller's thread.
//
// Records cross between the two threads through mutex-protected queues.
// The owner calls pump() (DisplaySession) from its own thread to apply
// what has arrived; replies the engine produces are queued and sent by the
// I/O thread.
#pragma once

#include "host/host_config.hpp"
#include "tn5250/engine.hpp"
#include "tn5250/telnet.hpp"
#include "tn5250/transport.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tn34xx {
namespace tn5250 {

enum class ConnState { Idle, Connecting, Negotiating, Ready, Closed, Failed };
const char* conn_state_name(ConnState s);

// NEW-ENVIRON / TERMINAL-TYPE settings for a configuration. Sends DEVNAME
// only when configured, KBDTYPE/CODEPAGE/CHARSET when known for the code
// page (or given in env), then the remaining env entries; RFC 1572's
// standard names (USER, JOB, ACCT, PRINTER, SYSTEMTYPE, DISPLAY) go as
// VAR, everything else as USERVAR.
TelnetSettings telnet_settings_for(const Tn5250Config& cfg);

class Client : public DisplaySession {
public:
    using TransportFactory = std::function<std::unique_ptr<Transport>(std::string& error)>;

    explicit Client(Tn5250Config cfg, TransportFactory factory = nullptr);
    ~Client() override;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Validate the configuration and start the I/O thread.
    bool start(std::string& error);
    void stop();
    ConnState state() const { return state_.load(); }
    std::string last_error() const;
    // Negotiation and record log lines since the last call.
    std::vector<std::string> take_log();

    Engine& engine() { return engine_; }
    const Engine& engine() const { return engine_; }

    // DisplaySession: everything but pump() forwards to the engine.
    void pump() override;
    int rows() const override { return engine_.rows(); }
    int cols() const override { return engine_.cols(); }
    const Cell& cell(int r, int c) const override { return engine_.cell(r, c); }
    int cursor_row() const override { return engine_.cursor_row(); }
    int cursor_col() const override { return engine_.cursor_col(); }
    Indicators indicators() const override { return engine_.indicators(); }
    uint64_t generation() const override { return engine_.generation(); }
    Rect take_dirty() override { return engine_.take_dirty(); }
    unsigned take_alarms() override { return engine_.take_alarms(); }
    void snapshot(ScreenSnapshot& out) const override { engine_.snapshot(out); }
    KeyResult key_data(uint8_t ebcdic) override { return engine_.key_data(ebcdic); }
    KeyResult key_function(Key key) override { return engine_.key_function(key); }
    KeyResult key_aid(Aid aid) override { return engine_.key_aid(aid); }
    void set_record_sink(RecordSink sink) override;

private:
    void run();
    void log(const std::string& line);
    void queue_outbound(const Record& rec);

    Tn5250Config cfg_;
    TransportFactory factory_;
    Engine engine_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<ConnState> state_{ConnState::Idle};

    mutable std::mutex mu_;
    std::deque<Record> inbound_;
    std::deque<std::vector<uint8_t>> outbound_;   // framed, ready to send
    std::vector<std::string> log_;
    std::string error_;
};

} // namespace tn5250
} // namespace tn34xx
