// SPDX-License-Identifier: GPL-3.0-or-later
// Application configuration: defaults, then a TOML file, then the command
// line (later sources override earlier ones).
#pragma once

#include "bus/bus.hpp"
#include "host/host_config.hpp"
#include "input/keyboard_ids.hpp"   // resolve_keyboard_id, keyboard_layout_list
#include "machine/machine.hpp"

#include <cstdint>
#include <string>

namespace em34xx {

struct AppConfig {
    // Machine
    std::string model = "3477";
    std::string rom_dir = "roms";
    bool allow_bad_rom = false;
    UnmappedPolicy unmapped = UnmappedPolicy::Stop;
    unsigned speed_percent = 100;   // 0 = unthrottled

    // Unit options: variant, phosphor and keyboard
    TerminalOptions terminal;
    // --display / [display] subtype of older versions: "green", "amber",
    // "paperwhite" or "color"; "" = not given. Application::configure()
    // turns it into the model's variant and phosphor (select_legacy_display).
    std::string legacy_display;

    // Front end
    bool crt = true;              // CRT scan-line presentation
    double crt_beam = 0.30;       // beam sigma in line pitches
    bool headless = false;        // no window even when the executable has one
    int scale = 1;

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


bool load_config_file(const std::string& path, AppConfig& cfg, std::string& error);

// Parses argv; loads --config FILE first so that other options override it.
bool parse_command_line(int argc, char** argv, AppConfig& cfg, std::string& error);

std::string usage_text(const char* argv0);

// Write the effective configuration back out as TOML.
std::string config_to_toml(const AppConfig& cfg);

} // namespace em34xx
