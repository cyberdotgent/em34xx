// Application configuration: defaults, then a TOML file, then the command
// line (later sources override earlier ones).
#pragma once

#include "bus/bus.hpp"
#include "host/host_config.hpp"
#include "machine/machine.hpp"

#include <cstdint>
#include <string>

namespace tn34xx {

struct AppConfig {
    // Machine
    std::string model = "3477";
    std::string rom_dir = "roms";
    bool allow_bad_rom = false;
    UnmappedPolicy unmapped = UnmappedPolicy::Stop;
    unsigned speed_percent = 100;   // 0 = unthrottled

    // Unit options: display subtype and keyboard (same ROM for all of them)
    TerminalOptions terminal;

    // Front end
    bool headless = false;
    int scale = 2;

    // Monitor
    std::string rc_file;
    bool batch = false;             // run rc_file, then exit
    bool start_stopped = false;
    std::string history_file;

    // GDB
    bool gdb = false;
    uint16_t gdb_port = 1234;
    bool gdb_wait = false;

    // Logging
    std::string log_spec;
    std::string log_file;

    // Host side
    TwinaxConfig twinax;
    Tn5250Config tn5250;

    // One-shot actions
    bool show_help = false;
    bool show_version = false;
    bool list_models = false;
    bool check_roms = false;
    bool list_keyboards = false;
};

// Check the keyboard type/layout and work out the two Read ID bytes the
// emulated keyboard reports (an explicit keyboard_id replaces the second).
bool resolve_keyboard_id(const TerminalOptions& t, uint8_t& id1, uint8_t& id2, std::string& error);
std::string keyboard_layout_list();

bool load_config_file(const std::string& path, AppConfig& cfg, std::string& error);

// Parses argv; loads --config FILE first so that other options override it.
bool parse_command_line(int argc, char** argv, AppConfig& cfg, std::string& error);

std::string usage_text(const char* argv0);

// Write the effective configuration back out as TOML.
std::string config_to_toml(const AppConfig& cfg);

} // namespace tn34xx
