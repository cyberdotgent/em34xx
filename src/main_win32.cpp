// SPDX-License-Identifier: GPL-3.0-or-later
// em34xxw: the Win32 GUI executable (GUI subsystem, end-user build: no
// monitor, no GDB stub). See docs/frontend-win32.md.
#include "app/application.hpp"
#include "frontend/win32/win32_frontend.hpp"

#include <exception>

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

namespace {

// DLLs loaded by name from here on come from the system folder, never from
// the current directory (a profile opened from a download folder or a share
// starts em34xxw there). SetDefaultDllDirectories exists on Windows 8 and
// on Windows 7 with KB2533623 only, so it is looked up at run time.
void harden_process() {
    SetDllDirectoryW(L"");
    using Fn = BOOL(WINAPI*)(DWORD);
    if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetDefaultDllDirectories"))))
        fn(LOAD_LIBRARY_SEARCH_SYSTEM32);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
}

// No std::string here: the exception may be std::bad_alloc.
int fatal(const char* what) {
    wchar_t text[512] = L"em34xxw ran into an unexpected error and has to close.";
    if (what && *what) {
        wchar_t detail[256] = {};
        MultiByteToWideChar(CP_UTF8, 0, what, -1, detail, int(sizeof detail / sizeof detail[0]) - 1);
        lstrcatW(text, L"\n\n");
        lstrcatW(text, detail);
    }
    MessageBoxW(nullptr, text, L"em34xx", MB_OK | MB_ICONERROR | MB_TASKMODAL);
    return 3;
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    harden_process();
    // Nothing may throw out of WinMain; the window and dialog procedures
    // catch their own exceptions (win32::guarded).
    try {
        em34xx::Win32Launch launch;
        launch.instance = instance;
        launch.show = show;
        if (!em34xx::win32_prepare(launch)) return launch.exit_code;
        em34xx::Application app(em34xx::make_win32_frontend(launch), nullptr);
        return em34xx::win32_run(app, launch);
    } catch (const std::exception& e) {
        return fatal(e.what());
    } catch (...) {
        return fatal(nullptr);
    }
}
