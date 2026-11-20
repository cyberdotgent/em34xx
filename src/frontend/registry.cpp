#include "frontend/registry.hpp"

#include "frontend/sdl/sdl_frontend.hpp"

namespace tn34xx {

const FrontendRegistry& builtin_frontends() {
    static const FrontendRegistry registry = {
        {"sdl", "SDL3 window", [] { return make_sdl_frontend(); }},
        {"none", "no window; the monitor runs on stdin/stdout", nullptr},
    };
    return registry;
}

} // namespace tn34xx
