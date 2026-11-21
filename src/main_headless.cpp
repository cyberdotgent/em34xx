// SPDX-License-Identifier: GPL-3.0-or-later
// tn34xx-headless: no window and no SDL; the monitor runs on stdin/stdout
// (or a script with --batch). Always built, also where SDL3 is not.
#include "app/application.hpp"
#include "devtools/dev_tools.hpp"

int main(int argc, char** argv) {
    tn34xx::Application app(nullptr, tn34xx::make_dev_tools());
    return app.main(argc, argv);
}
