// SPDX-License-Identifier: GPL-3.0-or-later
// Application settings of an end-user GUI build (settings.toml in the user
// folder, config/user_dirs.hpp): global, not per connection profile.
//
//   model = "3477"            terminal model to start (runnable_models())
//   [display]
//   subtype = "green"         green, amber, paperwhite or color
//   crt = true                CRT scan-line rendering
//   [emulator]
//   speed = 100               percent of the nominal clock, 0 = unthrottled
//   [tls]
//   min_version = "1.2"       default oldest TLS version for profiles that
//                             do not set their own
//   [window]
//   zoom = 1.0                1.0, 1.5 or 2.0: picture size relative to the
//                             presented frame, times the DPI scale
//   x, y, width, height       last window placement (pixels); absent = default
//   maximized = false
//
// Unknown keys are ignored and missing ones keep their defaults, so the
// file survives version changes either way.
#pragma once

#include "host/host_config.hpp"
#include "machine/display_subtype.hpp"

#include <string>

namespace em34xx {

struct WindowPlacement {
    bool valid = false;
    int x = 0, y = 0, width = 0, height = 0;
    bool maximized = false;
};

struct AppSettings {
    std::string model = "3477";
    DisplaySubtype display = DisplaySubtype::Green;
    bool crt = true;
    unsigned speed_percent = 100;
    TlsVersion tls_min_version = TlsVersion::Tls1_2;
    double zoom = 1.0;
    WindowPlacement window;
};

// The zoom levels offered (View > Zoom); zoom values are snapped to these.
constexpr double kZoomLevels[] = {1.0, 1.5, 2.0};
double snap_zoom(double z);

bool parse_settings(const std::string& text, const std::string& origin, AppSettings& out, std::string& error);
std::string settings_to_toml(const AppSettings& s);
// A missing file is not an error (defaults).
bool load_settings(const std::string& path, AppSettings& out, std::string& error);
bool save_settings(const std::string& path, const AppSettings& s, std::string& error);

// Read / write a whole file with a UTF-8 path (wide on Windows). Writes go
// to a temporary file first and replace the target.
bool read_text_file(const std::string& path, std::string& out, std::string& error);
bool write_text_file(const std::string& path, const std::string& text, std::string& error);

} // namespace em34xx
