// Developer tooling for the SDL and headless executables: the monitor (with
// its probes, memory tools, unmapped report and screenshots), the replxx
// console, the GDB stub and start-up scripts. End-user GUI builds do not
// link this (tn34xx_devtools); see docs/architecture.md.
#pragma once

#include "app/application.hpp"

#include <memory>

namespace tn34xx {

std::unique_ptr<AppTooling> make_dev_tools();

} // namespace tn34xx
