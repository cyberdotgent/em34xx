// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/connect_args.hpp"

#include "util/strings.hpp"

namespace em34xx {

namespace {

// Options that take a value; everything else is a flag.
bool takes_value(const std::string& name) {
    static const char* const kValued[] = {"station", "device", "type",    "port",    "codepage", "ca",
                                          "env",     "tls-verify", "tls-pin", "tls-min", "tls-max"};
    for (const char* v : kValued)
        if (name == v) return true;
    return false;
}

} // namespace

bool parse_connect_args(const std::vector<std::string>& args, const AppConfig& base, AppConfig& out,
                        std::string& error) {
    if (args.size() < 2 || args[1].empty()) {
        error = "usage: connect HOST[:PORT] [tls] [station N] [device NAME] [type TERMTYPE] [option value ...]";
        return false;
    }
    // HOST[:PORT]; a bracketed IPv6 literal may carry a port as well.
    std::string host = args[1], port;
    if (!host.empty() && host[0] == '[') {
        const auto close = host.find(']');
        if (close == std::string::npos) { error = "connect: unterminated [IPv6 address]"; return false; }
        if (close + 1 < host.size()) {
            if (host[close + 1] != ':') { error = "connect: bad host '" + host + "'"; return false; }
            port = host.substr(close + 2);
        }
        host = host.substr(1, close - 1);
    } else if (host.find(':') != std::string::npos && host.find(':') == host.rfind(':')) {
        port = host.substr(host.find(':') + 1);
        host = host.substr(0, host.find(':'));
    }
    if (host.empty()) { error = "connect: no host name"; return false; }

    // Build command-line options and let the command-line parser apply them
    // on top of the start-up configuration.
    std::vector<std::string> cli = {"em34xx", "--tn5250-host", host};
    bool tls = false;
    bool port_given = !port.empty();
    if (port_given) { cli.push_back("--tn5250-port"); cli.push_back(port); }
    bool station_given = false;
    for (size_t i = 2; i < args.size(); ++i) {
        std::string name = to_lower(args[i]), value;
        bool has_value = false;
        const auto eq = name.find('=');
        if (eq != std::string::npos) {
            value = args[i].substr(eq + 1);
            name = name.substr(0, eq);
            has_value = true;
        }
        if (!has_value && takes_value(name)) {
            if (i + 1 >= args.size()) { error = "connect: " + name + " needs a value"; return false; }
            value = args[++i];
            has_value = true;
        }
        if (name == "tls") {
            tls = true;
            cli.push_back("--tn5250-tls");
        } else if (name == "station") {
            uint64_t n;
            if (!parse_uint(value, n) || n > 6) { error = "connect: station must be 0..6"; return false; }
            if (!station_given) cli.push_back("--station");
            else { error = "connect: give one station"; return false; }
            cli.push_back(value + ":display");
            station_given = true;
        } else if (name == "port") {
            port_given = true;
            cli.push_back("--tn5250-port");
            cli.push_back(value);
        } else {
            static const struct { const char* word; const char* option; } kAlias[] = {
                {"device", "--tn5250-device"}, {"type", "--tn5250-type"}, {"ca", "--tn5250-ca"}};
            std::string option = "--tn5250-" + name;
            for (const auto& a : kAlias)
                if (name == a.word) option = a.option;
            cli.push_back(option);
            if (has_value) cli.push_back(value);
        }
    }
    if (tls && !port_given) { cli.push_back("--tn5250-port"); cli.push_back("992"); }

    AppConfig cfg = base;
    // Only the host side changes; a station list from start-up stays unless
    // `station` replaces it.
    std::vector<char*> argv;
    for (std::string& s : cli) argv.push_back(&s[0]);
    if (!parse_command_line(int(argv.size()), argv.data(), cfg, error)) {
        error = "connect: " + error;
        return false;
    }
    if (station_given) {
        // The command line keeps a configured station's device name; carry it over.
        const unsigned addr = cfg.twinax.stations.back().address;
        for (const StationConfig& st : base.twinax.effective_stations())
            if (st.address == addr && st.type == StationType::Display) cfg.twinax.stations.back() = st;
        cfg.twinax.station_address = 0;
    }
    cfg.twinax.host = TwinaxHostKind::Tn5250;
    out = cfg;
    return true;
}

} // namespace em34xx
