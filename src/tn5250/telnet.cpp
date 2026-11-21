// SPDX-License-Identifier: GPL-3.0-or-later
#include "tn5250/telnet.hpp"

#include "util/strings.hpp"

namespace em34xx {
namespace tn5250 {

using namespace telnet;

const char* telnet::option_name(uint8_t opt) {
    switch (opt) {
    case OPT_BINARY: return "BINARY";
    case OPT_ECHO: return "ECHO";
    case OPT_SGA: return "SUPPRESS-GO-AHEAD";
    case OPT_TIMING_MARK: return "TIMING-MARK";
    case OPT_TTYPE: return "TERMINAL-TYPE";
    case OPT_EOR: return "END-OF-RECORD";
    case OPT_NAWS: return "NAWS";
    case OPT_NEW_ENVIRON: return "NEW-ENVIRON";
    default: return nullptr;
    }
}

namespace {

std::string opt_text(uint8_t opt) {
    const char* n = option_name(opt);
    return n ? n : format("option %u", opt);
}

const char* cmd_text(uint8_t cmd) {
    switch (cmd) {
    case WILL: return "WILL";
    case WONT: return "WONT";
    case DO: return "DO";
    case DONT: return "DONT";
    default: return "?";
    }
}

// Options we are willing to perform ourselves (answer DO with WILL).
bool local_supported(uint8_t opt) {
    return opt == OPT_BINARY || opt == OPT_EOR || opt == OPT_TTYPE || opt == OPT_NEW_ENVIRON;
}
// Options we let the server perform (answer WILL with DO).
bool remote_supported(uint8_t opt) { return opt == OPT_BINARY || opt == OPT_EOR || opt == OPT_SGA; }

// Values safe to show in the negotiation log; anything else is summarised.
bool loggable_variable(const std::string& name) {
    return name == "DEVNAME" || name == "KBDTYPE" || name == "CODEPAGE" || name == "CHARSET" ||
           name == "IBMSENDCONFREC";
}

void append_escaped_env(std::vector<uint8_t>& out, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = p[i];
        if (b == ENV_VAR || b == ENV_VALUE || b == ENV_ESC || b == ENV_USERVAR) out.push_back(ENV_ESC);
        if (b == IAC) out.push_back(IAC);
        out.push_back(b);
    }
}

} // namespace

TelnetCodec::TelnetCodec(TelnetSettings settings) : settings_(std::move(settings)) {}

std::vector<std::string> TelnetCodec::take_log() {
    std::vector<std::string> out;
    out.swap(log_);
    return out;
}

bool TelnetCodec::transparent() const {
    return local_[OPT_BINARY] && remote_[OPT_BINARY] && local_[OPT_EOR] && remote_[OPT_EOR] && ttype_sent_;
}

void TelnetCodec::send_cmd(std::vector<uint8_t>& reply, uint8_t cmd, uint8_t opt) {
    reply.push_back(IAC);
    reply.push_back(cmd);
    reply.push_back(opt);
    note(format("send %s %s", cmd_text(cmd), opt_text(opt).c_str()));
}

void TelnetCodec::receive(const uint8_t* data, size_t len, std::vector<uint8_t>& reply,
                          std::vector<std::vector<uint8_t>>& records) {
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        switch (state_) {
        case State::Data:
            if (b == IAC) state_ = State::Iac;
            else current_.push_back(b);
            break;
        case State::Iac:
            state_ = State::Data;
            switch (b) {
            case IAC: current_.push_back(IAC); break;
            case EOR:
                records.push_back(std::move(current_));
                current_.clear();
                break;
            case WILL: state_ = State::Will; break;
            case WONT: state_ = State::Wont; break;
            case DO: state_ = State::Do; break;
            case DONT: state_ = State::Dont; break;
            case SB: sb_.clear(); state_ = State::Sb; break;
            case NOP: break;
            default: note(format("recv IAC %u (ignored)", b)); break;
            }
            break;
        case State::Will: handle_command(WILL, b, reply); state_ = State::Data; break;
        case State::Wont: handle_command(WONT, b, reply); state_ = State::Data; break;
        case State::Do: handle_command(DO, b, reply); state_ = State::Data; break;
        case State::Dont: handle_command(DONT, b, reply); state_ = State::Data; break;
        case State::Sb:
            if (b == IAC) state_ = State::SbIac;
            else sb_.push_back(b);
            break;
        case State::SbIac:
            if (b == SE) {
                state_ = State::Data;
                handle_subnegotiation(reply);
            } else if (b == IAC) {
                sb_.push_back(IAC);
                state_ = State::Sb;
            } else {
                // RFC 854: IAC followed by anything else inside SB is
                // malformed; drop the subnegotiation and resync.
                note(format("malformed subnegotiation (IAC %u); dropped", b));
                state_ = State::Data;
            }
            break;
        }
    }
}

void TelnetCodec::handle_command(uint8_t cmd, uint8_t opt, std::vector<uint8_t>& reply) {
    note(format("recv %s %s", cmd_text(cmd), opt_text(opt).c_str()));
    switch (cmd) {
    case DO:
        if (opt == OPT_TIMING_MARK) {
            send_cmd(reply, WILL, opt);  // RFC 860: acknowledge, no state
        } else if (local_supported(opt)) {
            if (!local_[opt]) {
                local_[opt] = true;
                send_cmd(reply, WILL, opt);
            }
        } else {
            send_cmd(reply, WONT, opt);
        }
        break;
    case DONT:
        if (local_[opt]) {
            local_[opt] = false;
            send_cmd(reply, WONT, opt);
        }
        break;
    case WILL:
        if (remote_supported(opt)) {
            if (!remote_[opt]) {
                remote_[opt] = true;
                send_cmd(reply, DO, opt);
            }
        } else {
            send_cmd(reply, DONT, opt);
        }
        break;
    case WONT:
        if (remote_[opt]) {
            remote_[opt] = false;
            send_cmd(reply, DONT, opt);
        }
        break;
    default: break;
    }
}

void TelnetCodec::handle_subnegotiation(std::vector<uint8_t>& reply) {
    if (sb_.empty()) return;
    const uint8_t opt = sb_[0];
    if (opt == OPT_TTYPE && sb_.size() >= 2 && sb_[1] == TT_SEND) {
        note("recv SB TERMINAL-TYPE SEND");
        if (!local_[OPT_TTYPE]) {
            note("  (TERMINAL-TYPE not enabled; ignored)");
            return;
        }
        reply.insert(reply.end(), {IAC, SB, OPT_TTYPE, TT_IS});
        for (char c : settings_.terminal_type) {
            reply.push_back(uint8_t(c));
            if (uint8_t(c) == IAC) reply.push_back(IAC);
        }
        reply.insert(reply.end(), {IAC, SE});
        ttype_sent_ = true;
        note("send SB TERMINAL-TYPE IS " + settings_.terminal_type);
        return;
    }
    if (opt == OPT_NEW_ENVIRON && sb_.size() >= 2 && sb_[1] == ENV_SEND) {
        if (!local_[OPT_NEW_ENVIRON]) {
            note("recv SB NEW-ENVIRON SEND (not enabled; ignored)");
            return;
        }
        answer_environ(std::vector<uint8_t>(sb_.begin() + 2, sb_.end()), reply);
        return;
    }
    note(format("recv SB %s (%zu bytes, ignored)", opt_text(opt).c_str(), sb_.size() - 1));
}

void TelnetCodec::answer_environ(const std::vector<uint8_t>& req, std::vector<uint8_t>& reply) {
    // Parse the request list: (VAR|USERVAR) name ... with ESC quoting.
    struct Item { bool user; std::vector<uint8_t> name; };
    std::vector<Item> items;
    for (size_t i = 0; i < req.size();) {
        const uint8_t t = req[i++];
        if (t != ENV_VAR && t != ENV_USERVAR) continue;  // tolerate junk
        Item it{t == ENV_USERVAR, {}};
        while (i < req.size() && req[i] != ENV_VAR && req[i] != ENV_USERVAR) {
            if (req[i] == ENV_ESC && i + 1 < req.size()) ++i;
            it.name.push_back(req[i++]);
        }
        items.push_back(std::move(it));
    }

    std::string desc = "recv SB NEW-ENVIRON SEND";
    for (const auto& it : items) {
        std::string n(it.name.begin(), it.name.end());
        if (n.size() == 16 && n.compare(0, 8, "IBMRSEED") == 0) {
            server_seed_.assign(it.name.begin() + 8, it.name.end());
            n = "IBMRSEED<" + hex_bytes(server_seed_.data(), server_seed_.size()) + ">";
        }
        desc += it.user ? " USERVAR" : " VAR";
        if (!n.empty()) desc += " " + n;
    }
    note(desc);

    auto wanted = [&](const EnvVariable& v) {
        if (items.empty()) return true;
        for (const auto& it : items) {
            if (it.user != v.user) continue;
            if (it.name.empty()) return true;
            if (std::string(it.name.begin(), it.name.end()) == v.name) return true;
        }
        return false;
    };

    reply.insert(reply.end(), {IAC, SB, OPT_NEW_ENVIRON, ENV_IS});
    std::string sent = "send SB NEW-ENVIRON IS";
    for (const auto& v : settings_.environment) {
        if (!wanted(v)) continue;
        reply.push_back(v.user ? ENV_USERVAR : ENV_VAR);
        append_escaped_env(reply, reinterpret_cast<const uint8_t*>(v.name.data()), v.name.size());
        reply.push_back(ENV_VALUE);
        append_escaped_env(reply, v.value.data(), v.value.size());
        sent += v.user ? " USERVAR " : " VAR ";
        sent += v.name + "=";
        if (loggable_variable(v.name)) sent += std::string(v.value.begin(), v.value.end());
        else sent += format("(%zu bytes)", v.value.size());
    }
    reply.insert(reply.end(), {IAC, SE});
    note(sent);
}

std::vector<uint8_t> TelnetCodec::frame(const std::vector<uint8_t>& record) {
    std::vector<uint8_t> out;
    out.reserve(record.size() + 8);
    for (uint8_t b : record) {
        out.push_back(b);
        if (b == IAC) out.push_back(IAC);
    }
    out.push_back(IAC);
    out.push_back(EOR);
    return out;
}

} // namespace tn5250
} // namespace em34xx
