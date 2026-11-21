// Win32 front end (tn34xxw): a native GUI-subsystem window with menus,
// toolbar and status bar from resources (tn34xxw.rc), connection profiles,
// the ROM manager, the keymap editor and the TLS trust prompt. Windows 7
// and later; Unicode (W) APIs only. See docs/frontend-win32.md.
//
// main_win32.cpp: win32_prepare() (user folder, settings, ROMs, command
// line), then Application(make_win32_frontend(), nullptr) and win32_run().
#pragma once

#include "config/settings.hpp"
#include "config/user_dirs.hpp"
#include "frontend/frontend.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace tn34xx {

class Application;

struct Win32Launch {
    HINSTANCE instance = nullptr;
    int show = SW_SHOWDEFAULT;
    std::unique_ptr<UserDirs> dirs;
    AppSettings settings;
    std::string keymap_path;            // the global keymap file, or "" for the built-in PCOMM map
    std::string profile_path;           // a .tn34xx file from the command line
    std::vector<std::string> args;      // the Application's command line
    int exit_code = 0;
};

// The user folder (%APPDATA%\tn34xx, or TN34XX_USER_DIR), settings, the
// ROM manager on first start, the command line. False: end with
// launch.exit_code (errors were shown in message boxes).
bool win32_prepare(Win32Launch& launch);

std::unique_ptr<Frontend> make_win32_frontend(Win32Launch& launch);

// Application::start() with launch.args (errors in a message box), then
// Application::run(). Returns the exit code.
int win32_run(Application& app, Win32Launch& launch);

} // namespace tn34xx
