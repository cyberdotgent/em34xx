// SDL3 window: shows the machine's display and forwards host keystrokes.
// Must run on the main thread.
#pragma once

#include "present/crt_render.hpp"

#include <string>

namespace tn34xx {

class Emulator;

struct FrontendOptions {
    int scale = 1;
    std::string title;
    CrtParams crt;
};

// Runs until the window is closed or the emulator is told to quit. Returns
// false (with error set) if SDL could not be initialised.
bool run_sdl_frontend(Emulator& emu, const FrontendOptions& opts, std::string& error);

} // namespace tn34xx
