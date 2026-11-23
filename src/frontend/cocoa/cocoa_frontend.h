// SPDX-License-Identifier: GPL-3.0-or-later
// Cocoa front end (em34xx.app): a native macOS application with the menu
// bar, a unified toolbar, a status bar with the session indicators,
// connection profiles, the ROM manager, the keymap editor and the TLS trust
// prompts. macOS 11 and later, arm64 and x86_64. docs/frontend-cocoa.md.
//
// NSApplication owns the main thread's run loop, so the Application is
// driven step by step (Application::start(), then request_quit() and
// finish() from applicationShouldTerminate:) instead of Application::run().
#pragma once

namespace em34xx {

// The whole program: NSApplication, the launch (user folder, settings,
// first-start ROM check, the profile Finder or the command line names),
// the window, and the run loop. Returns the process exit code.
int cocoa_main(int argc, char** argv);

} // namespace em34xx
