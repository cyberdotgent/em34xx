#include "config/config.hpp"

#include "input/keyboard_ids.hpp"
#include "util/strings.hpp"

#include <toml++/toml.hpp>

#include <cstring>
#include <sstream>

namespace tn34xx {

namespace {

bool parse_port(const std::string& text, uint16_t& out) {
    uint64_t v;
    if (!parse_uint(text, v) || v == 0 || v > 65535) return false;
    out = uint16_t(v);
    return true;
}

bool parse_env(const std::string& text, Tn5250Config& tn, std::string& error) {
    const auto eq = text.find('=');
    if (eq == std::string::npos || eq == 0) {
        error = "expected NAME=VALUE, got '" + text + "'";
        return false;
    }
    tn.env[text.substr(0, eq)] = text.substr(eq + 1);
    return true;
}

template <class T>
bool get(const toml::table& t, const char* key, T& out) {
    if (auto v = t[key].value<T>()) { out = *v; return true; }
    return false;
}

} // namespace

bool load_config_file(const std::string& path, AppConfig& cfg, std::string& error) {
    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        std::ostringstream os;
        os << path << ": " << e.description() << " (line " << e.source().begin.line << ")";
        error = os.str();
        return false;
    }

    std::string s;
    int64_t i;
    bool b;

    get(root, "model", cfg.model);
    get(root, "rom_dir", cfg.rom_dir);
    get(root, "allow_bad_rom", cfg.allow_bad_rom);

    if (auto* t = root["emulator"].as_table()) {
        if (get(*t, "unmapped", s) && !parse_unmapped_policy(s, cfg.unmapped)) {
            error = path + ": emulator.unmapped must be stop, log or ignore";
            return false;
        }
        if (get(*t, "speed", i)) cfg.speed_percent = unsigned(i < 0 ? 0 : i);
        if (get(*t, "headless", b)) cfg.headless = b;
        if (get(*t, "scale", i)) cfg.scale = int(i);
        if (get(*t, "start_stopped", b)) cfg.start_stopped = b;
        if (get(*t, "straps", i)) {
            if (i < 0 || i > 0xFF) { error = path + ": emulator.straps must be 0..255"; return false; }
            cfg.terminal.straps = int(i);
        }
        get(*t, "eeprom", cfg.terminal.eeprom_path);
    }
    if (auto* t = root["display"].as_table()) {
        get(*t, "crt", cfg.crt);
        if (auto v = (*t)["crt_beam"].value<double>()) cfg.crt_beam = *v;
        if (get(*t, "subtype", s) && !parse_display_subtype(s, cfg.terminal.display)) {
            error = path + ": display.subtype must be green, amber, paperwhite or color";
            return false;
        }
    }
    if (auto* t = root["keyboard"].as_table()) {
        get(*t, "type", cfg.terminal.keyboard_type);
        get(*t, "layout", cfg.terminal.keyboard_layout);
        if (get(*t, "id", i)) {
            if (i < 0 || i > 0xFF) { error = path + ": keyboard.id must be 0..255"; return false; }
            cfg.terminal.keyboard_id = int(i);
        }
        get(*t, "shift_fkeys", cfg.terminal.shift_fkeys);
    }
    if (auto* t = root["monitor"].as_table()) {
        get(*t, "rc", cfg.rc_file);
        get(*t, "history", cfg.history_file);
    }
    if (auto* t = root["gdb"].as_table()) {
        get(*t, "enabled", cfg.gdb);
        if (get(*t, "port", i)) {
            if (i <= 0 || i > 65535) { error = path + ": gdb.port out of range"; return false; }
            cfg.gdb_port = uint16_t(i);
        }
        get(*t, "wait", cfg.gdb_wait);
    }
    if (auto* t = root["log"].as_table()) {
        get(*t, "levels", cfg.log_spec);
        get(*t, "file", cfg.log_file);
    }
    if (auto* t = root["twinax"].as_table()) {
        if (get(*t, "host", s) && !parse_twinax_host_kind(s, cfg.twinax.host)) {
            error = path + ": twinax.host must be none, script or tn5250";
            return false;
        }
        if (get(*t, "station_address", i)) cfg.twinax.station_address = unsigned(i);
        if (auto* list = (*t)["station"].as_array()) {
            cfg.twinax.stations.clear();
            for (const auto& node : *list) {
                const toml::table* st = node.as_table();
                if (!st) { error = path + ": [[twinax.station]] entries must be tables"; return false; }
                StationConfig sc;
                if (!get(*st, "address", i) || i < 0 || i > 6) {
                    error = path + ": twinax.station.address must be 0..6";
                    return false;
                }
                sc.address = unsigned(i);
                if (get(*st, "type", s) && !parse_station_type(s, sc.type)) {
                    error = path + ": twinax.station.type must be display or printer";
                    return false;
                }
                get(*st, "tn5250_device_name", sc.device_name);
                get(*st, "profile", sc.profile);
                cfg.twinax.stations.push_back(sc);
            }
        }
    }
    if (auto* t = root["tn5250"].as_table()) {
        get(*t, "host", cfg.tn5250.host);
        if (get(*t, "port", i)) {
            if (i <= 0 || i > 65535) { error = path + ": tn5250.port out of range"; return false; }
            cfg.tn5250.port = uint16_t(i);
        }
        get(*t, "device_name", cfg.tn5250.device_name);
        get(*t, "terminal_type", cfg.tn5250.terminal_type);
        get(*t, "tls", cfg.tn5250.tls);
        get(*t, "tls_verify", cfg.tn5250.tls_verify);
        get(*t, "ca_file", cfg.tn5250.ca_file);
        get(*t, "codepage", cfg.tn5250.codepage);
        if (auto* env = (*t)["env"].as_table()) {
            for (const auto& kv : *env) {
                if (auto v = kv.second.value<std::string>()) cfg.tn5250.env[std::string(kv.first.str())] = *v;
            }
        }
    }
    return true;
}

bool resolve_keyboard_id(const TerminalOptions& t, uint8_t& id1, uint8_t& id2, std::string& error) {
    KeyboardType type;
    if (!parse_keyboard_type(t.keyboard_type, type)) {
        error = "unknown keyboard type '" + t.keyboard_type + "' (use 122 or enhanced)";
        return false;
    }
    id1 = keyboard_id_first_byte(type);
    if (t.keyboard_id >= 0) {
        id2 = uint8_t(t.keyboard_id);
        return true;
    }
    if (!find_keyboard_layout(t.keyboard_layout)) {
        error = "unknown keyboard layout '" + t.keyboard_layout + "' (see --list-keyboards)";
        return false;
    }
    if (!keyboard_id_bytes(type, t.keyboard_layout, id1, id2)) {
        error = "no " + std::string(keyboard_type_name(type)) + " keyboard exists for layout '" +
                t.keyboard_layout + "'";
        return false;
    }
    return true;
}

std::string keyboard_layout_list() {
    std::string out = format("%-14s %-6s %-9s %s\n", "layout", "122", "enhanced", "description");
    for (const KeyboardLayoutInfo& l : keyboard_layouts()) {
        out += format("%-14s %-6s %-9s %s%s\n", l.name, l.id122 ? format("%02X", l.id122).c_str() : "-",
                      l.id_enhanced ? format("%02X", l.id_enhanced).c_str() : "-", l.description,
                      l.documented ? "" : " (inferred)");
    }
    return out;
}

std::string usage_text(const char* argv0) {
    std::string u = format("usage: %s [options]\n", argv0);
    u += R"(
Machine:
  -m, --model NAME          terminal model (default 3477; see --list-models)
  -r, --rom-dir DIR         directory holding ROM images (default ./roms)
      --allow-bad-rom       load ROM dumps that are known to be defective
      --unmapped POLICY     unexpected accesses: stop (default), log, ignore
      --speed PCT           percent of nominal clock; 0 = unthrottled
  -c, --config FILE         TOML configuration file

Unit options (one ROM serves all of these):
      --display SUBTYPE     green (default), amber, paperwhite (white), color
      --green, --amber, --paperwhite, --color   shorthands for --display
      --no-crt              sharp pixels instead of CRT scan-line rendering
      --keyboard TYPE       122 (terminal, default) or enhanced
      --keyboard-layout L   keyboard country/layout, selects the terminal's
                            language (default us; see docs/keyboard.md)
      --keyboard-id ID      override the country ID byte (hex)
      --no-shift-fkeys      do not enter F13-F24 as Shift+F1..F12 (122-key)
      --straps HEX          override the configuration strap byte (3477:
                            display type and boot mode; see docs/bringup-3477.md)
      --eeprom FILE         setup EEPROM image; created on the first write.
                            Without it the EEPROM is ephemeral (starts blank,
                            discarded at exit)
      --list-keyboards      list keyboard layouts and their IDs

Front end and monitor:
      --headless            no window; the monitor runs on stdin/stdout
      --scale N             window size factor (default 1, aspect-corrected)
      --rc FILE             run monitor commands from FILE at start-up
      --batch               exit after the rc file (implies --headless)
  -S, --stopped             start with the CPU stopped at the reset vector
      --history FILE        monitor history file

Debugging:
      --gdb[=PORT]          start a GDB remote stub (default port 1234)
      --gdb-wait            stay stopped until a debugger attaches
      --log SPEC            log levels, e.g. "info" or "bus=debug,mmio=trace"
      --log-file FILE       also write log output to FILE

Host link (configuration only; the TN5250 bridge is not implemented yet):
      --twinax-host KIND    none (default), script, tn5250
      --station ADDR[:TYPE[:DEVNAME]]
                            a station address the terminal answers on, 0..6;
                            TYPE display (default) or printer; repeatable,
                            e.g. --station 0:display --station 1:printer
      --station-address N   single display station address (default 0)
      --tn5250-host HOST    TN5250 server
      --tn5250-port PORT    (default 23)
      --tn5250-device NAME  device name (DEVNAME)
      --tn5250-type TYPE    terminal type for every display station (default:
                            derived, 3477 colour IBM-3477-FC, mono IBM-3477-FG)
      --tn5250-tls          use TLS
      --tn5250-no-verify    do not verify the server certificate
      --tn5250-ca FILE      CA bundle for TLS
      --tn5250-codepage CP  EBCDIC code page (default 37)
      --tn5250-env N=V      extra NEW-ENVIRON variable (repeatable)

Other:
      --list-models         list known models and exit
      --check-roms          verify the model's ROM images and exit
      --dump-config         print the effective configuration as TOML
  -h, --help                this text
  -V, --version             version
)";
    return u;
}

bool parse_command_line(int argc, char** argv, AppConfig& cfg, std::string& error) {
    // Pass 1: configuration file.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string file;
        if ((a == "-c" || a == "--config") && i + 1 < argc) file = argv[i + 1];
        else if (starts_with(a, "--config=")) file = a.substr(9);
        if (!file.empty() && !load_config_file(file, cfg, error)) return false;
    }

    // Pass 2: everything else.
    bool cli_stations = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string value;
        bool has_inline = false;
        if (starts_with(a, "--")) {
            const auto eq = a.find('=');
            if (eq != std::string::npos) {
                value = a.substr(eq + 1);
                a = a.substr(0, eq);
                has_inline = true;
            }
        }
        auto need = [&](std::string& out) -> bool {
            if (has_inline) { out = value; return true; }
            if (i + 1 >= argc) { error = "option " + a + " needs a value"; return false; }
            out = argv[++i];
            return true;
        };
        std::string v;

        if (a == "-h" || a == "--help") cfg.show_help = true;
        else if (a == "-V" || a == "--version") cfg.show_version = true;
        else if (a == "--list-models") cfg.list_models = true;
        else if (a == "--check-roms") cfg.check_roms = true;
        else if (a == "--list-keyboards") cfg.list_keyboards = true;
        else if (a == "-c" || a == "--config") { if (!need(v)) return false; }
        else if (a == "-m" || a == "--model") { if (!need(cfg.model)) return false; }
        else if (a == "-r" || a == "--rom-dir") { if (!need(cfg.rom_dir)) return false; }
        else if (a == "--allow-bad-rom") cfg.allow_bad_rom = true;
        else if (a == "--unmapped") {
            if (!need(v)) return false;
            if (!parse_unmapped_policy(v, cfg.unmapped)) { error = "--unmapped must be stop, log or ignore"; return false; }
        } else if (a == "--speed") {
            if (!need(v)) return false;
            uint64_t n;
            if (v == "max") n = 0;
            else if (!parse_uint(v, n)) { error = "--speed needs a percentage"; return false; }
            cfg.speed_percent = unsigned(n);
        } else if (a == "--display" || a == "--subtype") {
            if (!need(v)) return false;
            if (!parse_display_subtype(v, cfg.terminal.display)) {
                error = "--display must be green, amber, paperwhite or color";
                return false;
            }
        } else if (a == "--no-crt") cfg.crt = false;
        else if (a == "--green") cfg.terminal.display = DisplaySubtype::Green;
        else if (a == "--amber") cfg.terminal.display = DisplaySubtype::Amber;
        else if (a == "--paperwhite" || a == "--white") cfg.terminal.display = DisplaySubtype::PaperWhite;
        else if (a == "--color" || a == "--colour") cfg.terminal.display = DisplaySubtype::Color;
        else if (a == "--keyboard") { if (!need(cfg.terminal.keyboard_type)) return false; }
        else if (a == "--keyboard-layout") { if (!need(cfg.terminal.keyboard_layout)) return false; }
        else if (a == "--keyboard-id") {
            if (!need(v)) return false;
            uint64_t n;
            if (!parse_uint(v, n, 16) || n > 0xFF) { error = "--keyboard-id must be a hex byte"; return false; }
            cfg.terminal.keyboard_id = int(n);
        } else if (a == "--no-shift-fkeys") cfg.terminal.shift_fkeys = false;
        else if (a == "--straps") {
            if (!need(v)) return false;
            uint64_t n;
            if (!parse_uint(v, n, 16) || n > 0xFF) { error = "--straps must be a hex byte"; return false; }
            cfg.terminal.straps = int(n);
        } else if (a == "--eeprom") { if (!need(cfg.terminal.eeprom_path)) return false; }
        else if (a == "--headless") cfg.headless = true;
        else if (a == "--scale") {
            if (!need(v)) return false;
            uint64_t n;
            if (!parse_uint(v, n) || n < 1 || n > 8) { error = "--scale must be 1..8"; return false; }
            cfg.scale = int(n);
        } else if (a == "--rc") { if (!need(cfg.rc_file)) return false; }
        else if (a == "--batch") { cfg.batch = true; cfg.headless = true; }
        else if (a == "-S" || a == "--stopped") cfg.start_stopped = true;
        else if (a == "--history") { if (!need(cfg.history_file)) return false; }
        else if (a == "--gdb") {
            cfg.gdb = true;
            if (has_inline && !parse_port(value, cfg.gdb_port)) { error = "bad --gdb port"; return false; }
        } else if (a == "--gdb-wait") { cfg.gdb = true; cfg.gdb_wait = true; }
        else if (a == "--log") { if (!need(cfg.log_spec)) return false; }
        else if (a == "--log-file") { if (!need(cfg.log_file)) return false; }
        else if (a == "--twinax-host") {
            if (!need(v)) return false;
            if (!parse_twinax_host_kind(v, cfg.twinax.host)) { error = "--twinax-host must be none, script or tn5250"; return false; }
        } else if (a == "--station-address") {
            if (!need(v)) return false;
            uint64_t n;
            if (!parse_uint(v, n) || n > 6) { error = "--station-address must be 0..6"; return false; }
            cfg.twinax.station_address = unsigned(n);
        } else if (a == "--station") {
            if (!need(v)) return false;
            StationConfig sc;
            if (!parse_station_spec(v, sc, error)) return false;
            if (!cli_stations) cfg.twinax.stations.clear();  // the command line replaces the file's list
            cli_stations = true;
            cfg.twinax.stations.push_back(sc);
        } else if (a == "--tn5250-host") { if (!need(cfg.tn5250.host)) return false; }
        else if (a == "--tn5250-port") {
            if (!need(v)) return false;
            if (!parse_port(v, cfg.tn5250.port)) { error = "bad --tn5250-port"; return false; }
        } else if (a == "--tn5250-device") { if (!need(cfg.tn5250.device_name)) return false; }
        else if (a == "--tn5250-type") { if (!need(cfg.tn5250.terminal_type)) return false; }
        else if (a == "--tn5250-tls") cfg.tn5250.tls = true;
        else if (a == "--tn5250-no-verify") cfg.tn5250.tls_verify = false;
        else if (a == "--tn5250-ca") { if (!need(cfg.tn5250.ca_file)) return false; }
        else if (a == "--tn5250-codepage") { if (!need(cfg.tn5250.codepage)) return false; }
        else if (a == "--tn5250-env") {
            if (!need(v)) return false;
            if (!parse_env(v, cfg.tn5250, error)) return false;
        } else if (a == "--dump-config") {
            // handled by main after parsing
        } else {
            error = "unknown option '" + a + "' (see --help)";
            return false;
        }
    }
    return true;
}

std::string config_to_toml(const AppConfig& cfg) {
    toml::table root{
        {"model", cfg.model},
        {"rom_dir", cfg.rom_dir},
        {"allow_bad_rom", cfg.allow_bad_rom},
        {"emulator", toml::table{
            {"unmapped", unmapped_policy_name(cfg.unmapped)},
            {"speed", int64_t(cfg.speed_percent)},
            {"headless", cfg.headless},
            {"scale", int64_t(cfg.scale)},
            {"start_stopped", cfg.start_stopped},
            {"eeprom", cfg.terminal.eeprom_path},
        }},
        {"display", toml::table{{"subtype", display_subtype_name(cfg.terminal.display)},
                                {"crt", cfg.crt},
                                {"crt_beam", cfg.crt_beam}}},
        {"keyboard", toml::table{
            {"type", cfg.terminal.keyboard_type},
            {"layout", cfg.terminal.keyboard_layout},
            {"shift_fkeys", cfg.terminal.shift_fkeys},
        }},
        {"monitor", toml::table{{"rc", cfg.rc_file}, {"history", cfg.history_file}}},
        {"gdb", toml::table{{"enabled", cfg.gdb}, {"port", int64_t(cfg.gdb_port)}, {"wait", cfg.gdb_wait}}},
        {"log", toml::table{{"levels", cfg.log_spec}, {"file", cfg.log_file}}},
        {"twinax", toml::table{
            {"host", twinax_host_kind_name(cfg.twinax.host)},
            {"station_address", int64_t(cfg.twinax.station_address)},
        }},
    };
    toml::table env;
    for (const auto& kv : cfg.tn5250.env) env.insert(kv.first, kv.second);
    root.insert("tn5250", toml::table{
        {"host", cfg.tn5250.host},
        {"port", int64_t(cfg.tn5250.port)},
        {"device_name", cfg.tn5250.device_name},
        {"terminal_type", cfg.tn5250.terminal_type},
        {"tls", cfg.tn5250.tls},
        {"tls_verify", cfg.tn5250.tls_verify},
        {"ca_file", cfg.tn5250.ca_file},
        {"codepage", cfg.tn5250.codepage},
        {"env", env},
    });
    if (!cfg.twinax.stations.empty()) {
        toml::array stations;
        for (const StationConfig& sc : cfg.twinax.stations) {
            toml::table st{{"address", int64_t(sc.address)}, {"type", station_type_name(sc.type)}};
            if (!sc.device_name.empty()) st.insert("tn5250_device_name", sc.device_name);
            if (!sc.profile.empty()) st.insert("profile", sc.profile);
            stations.push_back(std::move(st));
        }
        root["twinax"].as_table()->insert("station", std::move(stations));
    }
    if (cfg.terminal.straps >= 0)
        root["emulator"].as_table()->insert("straps", int64_t(cfg.terminal.straps));
    if (cfg.terminal.keyboard_id >= 0)
        root["keyboard"].as_table()->insert("id", int64_t(cfg.terminal.keyboard_id));
    std::ostringstream os;
    os << root << "\n";
    return os.str();
}

} // namespace tn34xx
