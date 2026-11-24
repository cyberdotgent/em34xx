// SPDX-License-Identifier: GPL-3.0-or-later
// Qt 6 Widgets front end (Linux, FreeBSD): a main window with a menu bar
// and a status bar around the terminal's screen. Must run on the main
// thread. See docs/frontend-qt.md.
#pragma once

#include "frontend/frontend.hpp"

#include <memory>
#include <string>

namespace em34xx {

std::unique_ptr<Frontend> make_qt_frontend();

// Show an error that ended the program before the window existed (a bad
// option, missing ROM images...) in a message box, when standard error is
// not a terminal (started from a desktop launcher) and there is a display.
// The caller has already written it to standard error. Not after
// Frontend::run().
void qt_error_box(const std::string& message);

} // namespace em34xx
