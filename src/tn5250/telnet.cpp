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
    case OPT_TN3270E: return "TN3270E";
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
bool basic_local_supported(uint8_t opt) {
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
    if (tn3270e_active()) return true;
    return local_[OPT_BINARY] && remote_[OPT_BINARY] && local_[OPT_EOR] && remote_[OPT_EOR] && ttype_sent_;
}

bool TelnetCodec::local_supported(uint8_t opt) const {
    return basic_local_supported(opt) || (opt == OPT_TN3270E && settings_.tn3270e);
}

// DEVICE-TYPE REQUEST <type> [CONNECT <name>] (RFC 2355 section 7.1).
void TelnetCodec::send_device_type(std::vector<uint8_t>& reply, bool with_name) {
    reply.insert(reply.end(), {IAC, SB, OPT_TN3270E, E_DEVICE_TYPE, E_REQUEST});
    for (char c : settings_.terminal_type) reply.push_back(uint8_t(c));
    std::string text = "send SB TN3270E DEVICE-TYPE REQUEST " + settings_.terminal_type;
    name_requested_ = with_name && !settings_.device_name.empty();
    if (name_requested_) {
        reply.push_back(E_CONNECT);
        for (char c : settings_.device_name) reply.push_back(uint8_t(c));
        text += " CONNECT " + settings_.device_name;
    }
    reply.insert(reply.end(), {IAC, SE});
    note(text);
}

void TelnetCodec::handle_tn3270e(std::vector<uint8_t>& reply) {
    const std::vector<uint8_t>& b = sb_;
    if (!local_[OPT_TN3270E]) {
        note("recv SB TN3270E (not enabled; ignored)");
        return;
    }
    if (b.size() >= 3 && b[1] == E_SEND && b[2] == E_DEVICE_TYPE) {
        note("recv SB TN3270E SEND DEVICE-TYPE");
        send_device_type(reply, true);
        return;
    }
    if (b.size() >= 3 && b[1] == E_DEVICE_TYPE && b[2] == E_IS) {
        // IS <type> CONNECT <lu>
        size_t i = 3;
        e_device_type_.clear();
        e_lu_.clear();
        while (i < b.size() && b[i] != E_CONNECT) e_device_type_.push_back(char(b[i++]));
        if (i < b.size()) ++i;
        while (i < b.size()) e_lu_.push_back(char(b[i++]));
        note("recv SB TN3270E DEVICE-TYPE IS " + e_device_type_ + (e_lu_.empty() ? "" : " CONNECT " + e_lu_));
        // No functions: no BIND images, responses, SYSREQ or SCS data.
        reply.insert(reply.end(), {IAC, SB, OPT_TN3270E, E_FUNCTIONS, E_REQUEST, IAC, SE});
        note("send SB TN3270E FUNCTIONS REQUEST (none)");
        return;
    }
    if (b.size() >= 3 && b[1] == E_DEVICE_TYPE && b[2] == E_REJECT) {
        static const char* const kReason[] = {"CONN-PARTNER", "DEVICE-IN-USE", "INV-ASSOCIATE", "INV-NAME",
                                              "INV-DEVICE-TYPE", "TYPE-NAME-ERROR", "UNKNOWN-ERROR",
                                              "UNSUPPORTED-REQ"};
        const int reason = b.size() >= 5 && b[3] == E_REASON ? b[4] : -1;
        note(format("recv SB TN3270E DEVICE-TYPE REJECT %s",
                    reason >= 0 && reason < 8 ? kReason[reason] : format("reason %d", reason).c_str()));
        if (name_requested_) {
            // The name is taken or unknown: let the server choose.
            send_device_type(reply, false);
        } else {
            // Fall back to plain TN3270.
            local_[OPT_TN3270E] = false;
            send_cmd(reply, WONT, OPT_TN3270E);
        }
        return;
    }
    if (b.size() >= 3 && b[1] == E_FUNCTIONS && (b[2] == E_REQUEST || b[2] == E_IS)) {
        const bool request = b[2] == E_REQUEST;
        note(format("recv SB TN3270E FUNCTIONS %s (%zu functions)", request ? "REQUEST" : "IS", b.size() - 3));
        if (!request || b.size() == 3) {
            if (request) {
                reply.insert(reply.end(), {IAC, SB, OPT_TN3270E, E_FUNCTIONS, E_IS, IAC, SE});
                note("send SB TN3270E FUNCTIONS IS (none)");
            }
            functions_agreed_ = true;
            note("TN3270E in force" + (e_lu_.empty() ? std::string() : ", LU " + e_lu_));
        } else {
            // Counter-propose none of them (RFC 2355 section 7.2).
            reply.insert(reply.end(), {IAC, SB, OPT_TN3270E, E_FUNCTIONS, E_REQUEST, IAC, SE});
            note("send SB TN3270E FUNCTIONS REQUEST (none)");
        }
        return;
    }
    note(format("recv SB TN3270E (%zu bytes, ignored)", b.size() - 1));
}

void TelnetCodec::send_cmd(std::vector<uint8_t>& reply, uint8_t cmd, uint8_t opt) {
    reply.push_back(IAC);
    reply.push_back(cmd);
    reply.push_back(opt);
    note(format("send %s %s", cmd_text(cmd), opt_text(opt).c_str()));
}

void TelnetCodec::fail(const std::string& why) {
    error_ = why;
    note("error: " + why);
    std::vector<uint8_t>().swap(current_);
    std::vector<uint8_t>().swap(sb_);
}

void TelnetCodec::receive(const uint8_t* data, size_t len, std::vector<uint8_t>& reply,
                          std::vector<std::vector<uint8_t>>& records) {
    for (size_t i = 0; i < len && !failed(); ++i) {
        const uint8_t b = data[i];
        if (current_.size() >= kMaxRecord) {
            fail(format("record exceeds %zu bytes without IAC EOR", kMaxRecord));
            break;
        }
        if (sb_.size() >= kMaxSubnegotiation) {
            fail(format("subnegotiation exceeds %zu bytes without IAC SE", kMaxSubnegotiation));
            break;
        }
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
    if (opt == OPT_TN3270E) {
        handle_tn3270e(reply);
        return;
    }
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
