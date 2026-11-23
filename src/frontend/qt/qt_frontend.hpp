// SPDX-License-Identifier: GPL-3.0-or-later
// Qt 6 Widgets front end (Linux, FreeBSD): a main window with a menu bar
// and a status bar around the terminal's screen. Must run on the main
// thread. See docs/frontend-qt.md.
#pragma once

#include "frontend/frontend.hpp"

#include <memory>

namespace em34xx {

std::unique_ptr<Frontend> make_qt_frontend();

} // namespace em34xx
