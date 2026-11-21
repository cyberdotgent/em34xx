// SPDX-License-Identifier: GPL-3.0-or-later
// em34xx: the SDL3 executable, with the developer tooling (monitor, console,
// GDB stub). --headless runs it without a window.
#include "app/application.hpp"
#include "devtools/dev_tools.hpp"
#include "frontend/sdl/sdl_frontend.hpp"

int main(int argc, char** argv) {
    em34xx::Application app(em34xx::make_sdl_frontend(), em34xx::make_dev_tools());
    return app.main(argc, argv);
}
