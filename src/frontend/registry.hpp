// The front ends built into this executable. Native front ends (Win32,
// Cocoa) are added here, each behind its own build option.
#pragma once

#include "frontend/frontend.hpp"

namespace tn34xx {

// "sdl" (when built) and "none"; the first entry is the default.
const FrontendRegistry& builtin_frontends();

} // namespace tn34xx
