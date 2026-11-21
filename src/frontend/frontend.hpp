// SPDX-License-Identifier: GPL-3.0-or-later
// The contract between the application and a front end (the window that
// shows the terminal's screen and takes the host keyboard).
//
// A front end owns the UI loop on the main thread. It gets the Application:
// Application::host_link() for connect / disconnect / status, power_on(),
// power_off(), reset() and the settings, and Application::emulator() for
// everything to do with the machine:
//   - the picture: Emulator::display_generation() / wait_display_change()
//     tell it when to redraw; Emulator::with_machine() + render_presented()
//     (present/display_profile.hpp) give the frame, crt_render() the tube;
//   - the keyboard: Machine::key_event() with a HostKeyEvent carrying the
//     key's USB HID usage (Keyboard/Keypad page 0x07), press and release,
//     no host auto-repeat (the terminal does its own typematic);
//   - lifetime: Emulator::request_quit() when the user closes the window;
//     return from run() once Emulator::quit_requested() or request_quit().
// Each front end is its own executable (tn34xx for SDL; tn34xxw for Win32
// and tn34xx.app for Cocoa later): its main() constructs the Frontend and
// hands it to Application. See docs/architecture.md ("Front ends").
#pragma once

#include "present/crt_render.hpp"

#include <string>

namespace tn34xx {

class Application;

struct FrontendOptions {
    int scale = 1;          // initial window size factor
    std::string title;      // window title (the front end appends run state)
    CrtParams crt;          // CRT presentation
};

class Frontend {
public:
    virtual ~Frontend() = default;
    virtual const char* name() const = 0;

    // Run the UI loop on the calling thread (the main thread) until the
    // user closes the window, the emulator is told to quit, or
    // request_quit() is called. Returns false, with error set, when the
    // front end could not start (no display, no video driver...).
    // opts: Application::frontend_options() (title, scale, CRT settings).
    virtual bool run(Application& app, const FrontendOptions& opts, std::string& error) = 0;

    // Make run() return soon. Safe from any thread.
    virtual void request_quit() = 0;
    // Window title and an optional status note shown after the run state.
    // Safe from any thread, before or during run().
    virtual void set_title(const std::string& title) = 0;
    virtual void set_status(const std::string& status) = 0;
};

} // namespace tn34xx
