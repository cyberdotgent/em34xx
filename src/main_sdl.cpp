// tn34xx: hardware emulator for IBM 34xx-family 5250 terminals.
//
// The entry point only picks the parts linked into this executable (the
// front ends and the terminal console); src/app/application.hpp does the
// rest.
#include "app/application.hpp"
#include "app/console.hpp"
#include "frontend/registry.hpp"

int main(int argc, char** argv) {
    tn34xx::Application app(tn34xx::builtin_frontends(), tn34xx::make_terminal_console);
    return app.main(argc, argv);
}
