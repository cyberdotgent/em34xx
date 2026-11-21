// SPDX-License-Identifier: GPL-3.0-or-later
// Link check for end-user GUI builds: a minimal application that links only
// the runtime libraries (em34xx_runtime and below), with a stub front end,
// no developer tooling. Building it proves the runtime does not need the
// monitor, the console or the GDB stub; running it checks that tooling
// options are refused.
#include "app/application.hpp"
#include "app/host_link.hpp"
#include "machine/emulator.hpp"

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

class StubFrontend : public em34xx::Frontend {
public:
    const char* name() const override { return "stub"; }
    bool run(em34xx::Application& app, const em34xx::FrontendOptions&, std::string&) override {
        // What a GUI does: status, then quit.
        std::printf("controller: %s\n", app.host_link().controller_kind().c_str());
        app.request_quit(0);
        return true;
    }
    void request_quit() override {}
    void set_title(const std::string&) override {}
    void set_status(const std::string&) override {}
};

int run_app(std::vector<const char*> args) {
    em34xx::Application app(std::make_unique<StubFrontend>(), nullptr);
    return app.main(int(args.size()), const_cast<char**>(args.data()));
}

} // namespace

int main() {
    int failures = 0;
    if (run_app({"em34xx", "--version"}) != 0) {
        std::puts("FAIL: --version");
        ++failures;
    }
    // Tooling options must be refused without the developer tooling.
    for (const char* opt : {"--gdb", "--headless", "--batch", "-S"}) {
        if (run_app({"em34xx", opt}) != 2) {
            std::printf("FAIL: %s accepted without tooling\n", opt);
            ++failures;
        }
    }
    std::puts(failures ? "runtime-only link check FAILED" : "runtime-only link check passed");
    return failures ? 1 : 0;
}
