// Client-side telnet layer for TN5250.
//
// Pure state machine: bytes from the server go in, negotiation replies and
// complete (unescaped) logical records come out. No sockets, so the
// negotiation can be tested from transcripts.
//
// Options (RFC 855 option negotiation, answered without loops as RFC 854
// requires: we only reply when an option's state changes):
//   BINARY (RFC 856), END-OF-RECORD (RFC 885)    accepted in both directions
//   TERMINAL-TYPE (RFC 1091)                     WILL; answers SEND with IS <type>
//   NEW-ENVIRON (RFC 1572, IBM usage RFC 4777)   WILL; answers SEND with IS
//   TIMING-MARK (RFC 860)                        DO is answered WILL, stateless
//   anything else                                refused (WONT / DONT)
//
// NEW-ENVIRON: we send only what the configuration provides (DEVNAME when
// configured, KBDTYPE/CODEPAGE/CHARSET, and explicit extras). We never send
// USER or a password. An IBMRSEED<seed> request (RFC 4777 section 5) is
// recorded but not answered with IBMSUBSPW: password substitution needs DES
// or SHA-1 and a password, and auto sign-on is not implemented.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tn34xx {
namespace tn5250 {

namespace telnet {
constexpr uint8_t IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, GA = 249,
                  EL = 248, EC = 247, AYT = 246, AO = 245, IP = 244, BRK = 243, DM = 242, NOP = 241,
                  SE = 240, EOR = 239;
constexpr uint8_t OPT_BINARY = 0, OPT_ECHO = 1, OPT_SGA = 3, OPT_TIMING_MARK = 6, OPT_TTYPE = 24,
                  OPT_EOR = 25, OPT_NAWS = 31, OPT_NEW_ENVIRON = 39;
// TERMINAL-TYPE and NEW-ENVIRON subcommands.
constexpr uint8_t TT_IS = 0, TT_SEND = 1;
constexpr uint8_t ENV_IS = 0, ENV_SEND = 1, ENV_INFO = 2;
constexpr uint8_t ENV_VAR = 0, ENV_VALUE = 1, ENV_ESC = 2, ENV_USERVAR = 3;
const char* option_name(uint8_t opt);
} // namespace telnet

struct EnvVariable {
    std::string name;
    std::vector<uint8_t> value;
    bool user = true;   // USERVAR (true) or VAR (false)
};

struct TelnetSettings {
    std::string terminal_type = "IBM-3477-FC";
    std::vector<EnvVariable> environment;
};

class TelnetCodec {
public:
    explicit TelnetCodec(TelnetSettings settings);

    // Process bytes from the server. Negotiation replies are appended to
    // `reply`; complete logical records (unescaped) to `records`.
    void receive(const uint8_t* data, size_t len, std::vector<uint8_t>& reply,
                 std::vector<std::vector<uint8_t>>& records);

    // Escape a logical record (IAC doubling) and terminate it with IAC EOR.
    static std::vector<uint8_t> frame(const std::vector<uint8_t>& record);

    // BINARY and EOR agreed both ways and the terminal type sent.
    bool transparent() const;
    bool local_enabled(uint8_t opt) const { return local_[opt]; }
    bool remote_enabled(uint8_t opt) const { return remote_[opt]; }
    bool terminal_type_sent() const { return ttype_sent_; }
    // Server seed from an IBMRSEED request, empty if none was seen.
    const std::vector<uint8_t>& server_seed() const { return server_seed_; }

    // Human-readable negotiation log ("recv DO TERMINAL-TYPE", ...).
    const std::vector<std::string>& log() const { return log_; }
    std::vector<std::string> take_log();

private:
    enum class State { Data, Iac, Will, Wont, Do, Dont, Sb, SbIac };

    void handle_command(uint8_t cmd, uint8_t opt, std::vector<uint8_t>& reply);
    void handle_subnegotiation(std::vector<uint8_t>& reply);
    void answer_environ(const std::vector<uint8_t>& request, std::vector<uint8_t>& reply);
    void send_cmd(std::vector<uint8_t>& reply, uint8_t cmd, uint8_t opt);
    void note(const std::string& line) { log_.push_back(line); }

    TelnetSettings settings_;
    State state_ = State::Data;
    std::vector<uint8_t> current_;     // record being assembled
    std::vector<uint8_t> sb_;          // subnegotiation being assembled
    bool local_[256] = {};
    bool remote_[256] = {};
    bool ttype_sent_ = false;
    std::vector<uint8_t> server_seed_;
    std::vector<std::string> log_;
};

} // namespace tn5250
} // namespace tn34xx
