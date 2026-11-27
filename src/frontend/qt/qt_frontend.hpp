// SPDX-License-Identifier: GPL-3.0-or-later
// Qt 6 Widgets front end (em34xx-qt, Linux and FreeBSD): a main window with
// menus, a toolbar and a status bar, connection profiles, the ROM manager,
// the keymap editor and the TLS trust prompts, after em34xxw
// (docs/frontend-win32.md). Must run on the main thread. See
// docs/frontend-qt.md.
//
// main_qt.cpp: qt_prepare() (display, user folder, settings, ROMs, command
// line), then Application(make_qt_frontend(), nullptr) and qt_run().
#pragma once

#include "config/settings.hpp"
#include "config/user_dirs.hpp"
#include "frontend/frontend.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QApplication;

namespace em34xx {

class Application;

struct QtLaunch {
    QtLaunch();
    ~QtLaunch();
    std::unique_ptr<QApplication> qapp;
    std::unique_ptr<UserDirs> dirs;
    AppSettings settings;
    std::string keymap_path;            // the global keymap file, or "" for the built-in PCOMM map
    std::string profile_path;           // a .em34xx file from the command line
    // The profile start-up would open (command line or startup profile) was
    // for a model whose ROM images the user did not provide: open none.
    bool skip_start_profile = false;
    std::vector<std::string> args;      // the Application's command line
    int exit_code = 0;
    // Tests (tests/qt_gui_test.cpp): runs once the QApplication exists,
    // before any dialog of the launch.
    std::function<void()> on_qapp;
};

// The command line (`em34xx-qt [PROFILE.em34xx]`, --help, --version), the
// display, the user folder, settings, the ROM manager on first start.
// False: end with launch.exit_code (errors were shown in message boxes, or
// on standard error when there is no display).
bool qt_prepare(int argc, char** argv, QtLaunch& launch);

std::unique_ptr<Frontend> make_qt_frontend(QtLaunch& launch);

// Application::start() with launch.args (errors in a message box), then
// Application::run(). Returns the exit code.
int qt_run(Application& app, QtLaunch& launch);

// An error that ends the program, in a message box when there is a display
// (also written to standard error).
void qt_fatal_box(const std::string& message);

} // namespace em34xx
