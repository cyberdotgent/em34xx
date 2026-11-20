// The tn34xx application: configuration -> ROMs -> machine -> Twinax host or
// TN5250 bridge -> monitor (interactive console or script) -> GDB stub ->
// run loop -> shutdown and exit code. main() is a thin wrapper around it.
//
// Two ways to drive it:
//   - Application::main(argc, argv) does everything; the selected front end
//     (or, headless, the monitor console) owns the main thread.
//   - Front ends that must own the process's run loop themselves (Cocoa's
//     NSApplicationMain, a Win32 message loop started elsewhere) call
//     start(), then start_console() if they want the terminal console,
//     drive emulator() from their loop, and call request_quit() and
//     finish() when they are done.
#pragma once

#include "app/console.hpp"
#include "config/config.hpp"
#include "frontend/frontend.hpp"
#include "host/twinax_link.hpp"

#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace tn34xx {

class Emulator;
class GdbStub;
class Model;
class Monitor;

class Application {
public:
    // Builds the interactive console when stdin is a terminal; may be null
    // (commands are then read line by line from stdin).
    using ConsoleFactory =
        std::function<std::unique_ptr<Console>(Monitor& monitor, Emulator& emu, const std::string& history_file)>;

    Application(FrontendRegistry frontends, ConsoleFactory console);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // The whole program; returns the process exit code.
    int main(int argc, char** argv);

    // --- step by step ---------------------------------------------------
    // Parse the configuration, build and start the machine, the host link,
    // the monitor and the GDB stub, and run the start-up script. Returns
    // false when the program should end now (one-shot actions such as
    // --help, errors, --batch); exit_code() then holds the status.
    bool start(int argc, char** argv);
    // Hand the main thread to the selected front end, or to the monitor when
    // headless, until the user quits; then finish(). Returns the exit code.
    int run();
    // Start the interactive console on a helper thread (stdin a terminal
    // only); stop_console() wakes and joins it.
    void start_console();
    void stop_console();
    // Ask everything to wind down with this exit code; any thread.
    void request_quit(int exit_code);
    // Stop the console, the GDB stub and the emulation thread. Returns the
    // exit code. Idempotent.
    int finish();

    int exit_code() const { return exit_code_; }
    const AppConfig& config() const { return cfg_; }
    Emulator& emulator() { return *emu_; }
    Monitor& monitor() { return *monitor_; }
    // Window options for the configured model and display.
    FrontendOptions frontend_options() const;
    // The selected front end entry (create == null when headless).
    const FrontendEntry& frontend_entry() const { return *frontend_entry_; }

private:
    bool configure(int argc, char** argv);
    bool build_machine();
    bool start_debuggers();
    int run_headless();
    int run_frontend();
    bool fail(const std::string& message, int code = 2);

    FrontendRegistry frontends_;
    ConsoleFactory console_factory_;

    AppConfig cfg_;
    const FrontendEntry* frontend_entry_ = nullptr;
    const Model* model_ = nullptr;
    Tn5250HostFactory bridge_;
    std::unique_ptr<Emulator> emu_;
    std::unique_ptr<Monitor> monitor_;
    std::unique_ptr<GdbStub> gdb_;
    std::unique_ptr<Console> console_;
    std::thread console_thread_;
    bool finished_ = false;
    int exit_code_ = 0;
};

} // namespace tn34xx
