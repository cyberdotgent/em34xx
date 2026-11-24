// SPDX-License-Identifier: GPL-3.0-or-later
// em34xx-qt: the Qt GUI executable for Linux and FreeBSD (end-user build:
// no monitor, no GDB stub). See docs/frontend-qt.md.
#include "app/application.hpp"
#include "frontend/qt/qt_frontend.hpp"

int main(int argc, char** argv) {
    em34xx::Application app(em34xx::make_qt_frontend(), nullptr);
    if (!app.start(argc, argv)) {
        if (!app.last_error().empty()) em34xx::qt_error_box(app.last_error());
        return app.exit_code();
    }
    return app.run();
}
