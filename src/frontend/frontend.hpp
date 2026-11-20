// The contract between the application and a front end (the window that
// shows the terminal's screen and takes the host keyboard).
//
// A front end owns the UI loop on the main thread. Everything it needs from
// the emulator goes through the Emulator it is given:
//   - the picture: Emulator::display_generation() / wait_display_change()
//     tell it when to redraw; Emulator::with_machine() + render_presented()
//     (present/display_profile.hpp) give the frame, crt_render() the tube;
//   - the keyboard: Machine::key_event() with a HostKeyEvent carrying the
//     key's USB HID usage (Keyboard/Keypad page 0x07), press and release,
//     no host auto-repeat (the terminal does its own typematic);
//   - lifetime: Emulator::request_quit() when the user closes the window;
//     return from run() once Emulator::quit_requested() or request_quit().
// See docs/architecture.md ("Front ends") for how a native Win32 or Cocoa
// front end plugs in.
#pragma once

#include "present/crt_render.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tn34xx {

class Emulator;

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
    virtual bool run(Emulator& emu, const FrontendOptions& opts, std::string& error) = 0;

    // Make run() return soon. Safe from any thread.
    virtual void request_quit() = 0;
    // Window title and an optional status note shown after the run state.
    // Safe from any thread, before or during run().
    virtual void set_title(const std::string& title) = 0;
    virtual void set_status(const std::string& status) = 0;
};

// One selectable front end (--frontend NAME, TOML [frontend] kind). A null
// create means "no window": the application runs headless and the monitor
// owns the main thread.
struct FrontendEntry {
    std::string name;
    std::string description;
    std::function<std::unique_ptr<Frontend>()> create;
};

using FrontendRegistry = std::vector<FrontendEntry>;

inline const FrontendEntry* find_frontend(const FrontendRegistry& registry, const std::string& name) {
    for (const FrontendEntry& e : registry)
        if (e.name == name) return &e;
    return nullptr;
}

} // namespace tn34xx
