// SPDX-License-Identifier: GPL-3.0-or-later
// tn34xx: the SDL3 executable, with the developer tooling (monitor, console,
// GDB stub). --headless runs it without a window.
#include "app/application.hpp"
#include "devtools/dev_tools.hpp"
#include "frontend/sdl/sdl_frontend.hpp"

int main(int argc, char** argv) {
    tn34xx::Application app(tn34xx::make_sdl_frontend(), tn34xx::make_dev_tools());
    return app.main(argc, argv);
}
