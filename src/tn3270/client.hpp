// SPDX-License-Identifier: GPL-3.0-or-later
// A TN3270 session: transport and telnet negotiation on an I/O thread, the
// 3270 engine on the owner's thread, as tn5250::Client does for 5250.
//
// Basic TN3270 (RFC 1576): BINARY, END-OF-RECORD and TERMINAL-TYPE, then
// 3270 data stream records. The session endpoint (host, port, TLS) uses the
// same configuration as a TN5250 session (Tn5250Config); the terminal type
// defaults to IBM-3279-2-E, a colour model 2 with the extended data stream.
//
// Records cross between the threads through bounded, mutex-protected
// queues; the owner calls pump() to apply what has arrived, and the
// replies the engine produces (or an attention key the owner sends) go out
// on the I/O thread.
#pragma once

#include "host/host_config.hpp"
#include "tn3270/engine.hpp"
#include "tn5250/telnet.hpp"
#include "tn5250/tls_trust.hpp"
#include "tn5250/transport.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace em34xx {
namespace tn3270 {

enum class ConnState { Idle, Connecting, Negotiating, Ready, Closed, Failed };
const char* conn_state_name(ConnState s);

class Client {
public:
    using TransportFactory = std::function<std::unique_ptr<tn5250::Transport>(std::string& error)>;
    static constexpr size_t kMaxQueuedBytes = 4u << 20;
    static constexpr size_t kMaxLogLines = 500;
    static constexpr const char* kDefaultTerminalType = "IBM-3279-2-E";

    explicit Client(Tn5250Config cfg, TransportFactory factory = nullptr);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void set_tls_trust_handler(tn5250::TlsTrustHandler fn) { trust_ = std::move(fn); }
    bool start(std::string& error);
    void stop();
    ConnState state() const { return state_.load(); }
    // TN3270E in force (RFC 2355), and the LU name the server assigned
    // (empty with plain TN3270, or when the server names none).
    bool tn3270e() const { return tn3270e_.load(); }
    std::string lu_name() const;
    std::string last_error() const;
    std::vector<std::string> take_log();

    // Owner thread: apply the records that have arrived.
    void pump();
    Engine& engine() { return engine_; }
    const Engine& engine() const { return engine_; }

    // Owner thread: operator actions. aid() sends the inbound record.
    bool type(uint8_t ebcdic) { return engine_.type(ebcdic); }
    void key(Key k) { engine_.key(k); }
    void aid(uint8_t aid);
    // The ATTN key: a Telnet IP command [documented: RFC 2355 section 11].
    void attn() { send_command(tn5250::telnet::IP, "ATTN"); }
    // The SYSREQ key: a Telnet AO command [documented: RFC 2355 section
    // 10.5.2, for the SYSREQ function; a server that has not agreed to it
    // ignores the command]. The keyboard is not locked.
    void sysreq() { send_command(tn5250::telnet::AO, "SYSREQ"); }

private:
    void run();
    void run_session();
    void fail(const std::string& why);
    void log(const std::string& line);
    void log_locked(const std::string& line);
    void queue_outbound(const std::vector<uint8_t>& record);
    void send_command(uint8_t command, const char* what);

    Tn5250Config cfg_;
    TransportFactory factory_;
    tn5250::TlsTrustHandler trust_;
    Engine engine_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<ConnState> state_{ConnState::Idle};
    std::atomic<bool> tn3270e_{false};
    std::string lu_;   // under mu_
    mutable std::mutex mu_;
    std::deque<std::vector<uint8_t>> inbound_;
    std::deque<std::vector<uint8_t>> outbound_;   // framed
    size_t inbound_bytes_ = 0;
    size_t outbound_bytes_ = 0;
    bool outbound_overflow_ = false;
    std::deque<std::string> log_;
    size_t log_dropped_ = 0;
    std::string error_;
};

} // namespace tn3270
} // namespace em34xx
