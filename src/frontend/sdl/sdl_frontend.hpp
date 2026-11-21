// SPDX-License-Identifier: GPL-3.0-or-later
// SDL3 front end: one self-contained window that shows the machine's
// display and forwards host keystrokes. Must run on the main thread.
#pragma once

#include "frontend/frontend.hpp"

#include <memory>

namespace em34xx {

std::unique_ptr<Frontend> make_sdl_frontend();

} // namespace em34xx
