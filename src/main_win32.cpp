// SPDX-License-Identifier: GPL-3.0-or-later
// tn34xxw: the Win32 GUI executable (GUI subsystem, end-user build: no
// monitor, no GDB stub). See docs/frontend-win32.md.
#include "app/application.hpp"
#include "frontend/win32/win32_frontend.hpp"

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    tn34xx::Win32Launch launch;
    launch.instance = instance;
    launch.show = show;
    if (!tn34xx::win32_prepare(launch)) return launch.exit_code;
    tn34xx::Application app(tn34xx::make_win32_frontend(launch), nullptr);
    return tn34xx::win32_run(app, launch);
}
