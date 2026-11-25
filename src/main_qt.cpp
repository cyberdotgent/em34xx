// SPDX-License-Identifier: GPL-3.0-or-later
// em34xx-qt: the Qt GUI executable for Linux and FreeBSD (end-user build:
// no monitor, no GDB stub). See docs/frontend-qt.md.
#include "app/application.hpp"
#include "frontend/qt/qt_frontend.hpp"

#include <exception>

int main(int argc, char** argv) {
    // Nothing may throw out of main; menu commands and dialogs catch their
    // own exceptions (qt::guarded).
    try {
        em34xx::QtLaunch launch;
        if (!em34xx::qt_prepare(argc, argv, launch)) return launch.exit_code;
        em34xx::Application app(em34xx::make_qt_frontend(launch), nullptr);
        return em34xx::qt_run(app, launch);
    } catch (const std::exception& e) {
        em34xx::qt_fatal_box(std::string("em34xx-qt ran into an unexpected error and has to close.\n\n") + e.what());
        return 3;
    } catch (...) {
        em34xx::qt_fatal_box("em34xx-qt ran into an unexpected error and has to close.");
        return 3;
    }
}
