// SPDX-License-Identifier: GPL-3.0-or-later
// Interactive monitor console (developer tooling): a line editor (replxx) in front of
// Monitor::execute. Lives outside the monitor library so that the emulator
// core, the monitor and the tests never link a line-editing library; native
// front ends may provide their own console window instead.
#pragma once

#include <memory>
#include <string>

namespace tn34xx {

class Emulator;
class Monitor;

class Console {
public:
    virtual ~Console() = default;
    // Prompt until "quit", EOF, or the emulator is told to quit. Runs on the
    // calling thread (the main thread headless, a helper thread otherwise).
    virtual void run() = 0;
    // Wake a prompt blocked waiting for input; safe from any thread.
    virtual void interrupt() = 0;
};

// The replxx terminal console, with history kept in history_file (empty:
// no history file).
std::unique_ptr<Console> make_terminal_console(Monitor& monitor, Emulator& emu, const std::string& history_file);

} // namespace tn34xx
