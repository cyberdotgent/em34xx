// SPDX-License-Identifier: GPL-3.0-or-later
// The tn34xx application runtime: configuration -> ROMs -> machine -> Twinax
// host or TN5250 bridge -> run loop -> shutdown and exit code.
//
// This is the lean part every executable links (tn34xx_runtime). Developer
// tooling (monitor, console, GDB stub, start-up scripts) is an optional
// AppTooling (src/devtools, tn34xx_devtools) that only the SDL `tn34xx` and
// `tn34xx-headless` executables plug in; end-user GUI builds (tn34xxw,
// tn34xx.app) run without it.
//
// Two ways to drive it:
//   - Application::main(argc, argv) does everything: the front end (or,
//     headless, the tooling's console) owns the main thread.
//   - Front ends that must own the process's run loop themselves (Cocoa's
//     NSApplicationMain) call start(), drive emulator() and the control API
//     below from their loop, then request_quit() and finish().
#pragma once

#include "config/config.hpp"
#include "frontend/frontend.hpp"
#include "host/twinax_link.hpp"

#include <memory>
#include <string>

namespace tn34xx {

class Application;
class Emulator;
class HostLink;
class Model;
struct StopEvent;

// Developer tooling plugged into the application (src/devtools/dev_tools.hpp).
class AppTooling {
public:
    virtual ~AppTooling() = default;
    // Whether the command line asked for a tooling feature (--rc, --batch,
    // --gdb, -S...). Without tooling such options are an error.
    static bool requested(const AppConfig& cfg);
    // The emulator is built and its thread started, CPU not yet running.
    // Set up (GDB stub, start-up script) and decide whether to resume.
    // Return false to end the program now with exit_code (--batch).
    virtual bool start(Application& app, int& exit_code) = 0;
    // Called on the emulation thread whenever a run ends.
    virtual void on_stop(const StopEvent& ev) = 0;
    // Own the main thread when there is no window, until the user quits.
    virtual void run_headless(Application& app) = 0;
    // Run alongside a front end (an interactive console thread) / stop that.
    virtual void start_background(Application& app) = 0;
    virtual void stop_background() = 0;
    // Wind down (GDB stub); called once from Application::finish().
    virtual void shutdown() = 0;
    // Exit status when the emulator was not told to quit with a code
    // (failed monitor assertions -> 1).
    virtual int exit_status() const = 0;
};

class Application {
public:
    // frontend: the window, or null for a headless-only executable.
    // tooling: developer tooling, or null for an end-user build.
    Application(std::unique_ptr<Frontend> frontend, std::unique_ptr<AppTooling> tooling);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // The whole program; returns the process exit code.
    int main(int argc, char** argv);

    // --- step by step ---------------------------------------------------
    // Parse the configuration, build and start the machine and the host
    // link (and the tooling). Returns false when the program should end now
    // (one-shot actions such as --help, errors, --batch); exit_code() then
    // holds the status.
    bool start(int argc, char** argv);
    // Hand the main thread to the front end, or to the tooling when
    // headless, until the user quits; then finish(). Returns the exit code.
    int run();
    // Ask everything to wind down with this exit code; any thread.
    void request_quit(int exit_code);
    // Stop the tooling and the emulation thread; returns the exit code.
    // Idempotent.
    int finish();

    // --- control API for front ends ---------------------------------------
    // Connect / disconnect / status of the host link (TN5250 bridge).
    HostLink& host_link() { return *host_link_; }
    // Power: on = reset and run, off = stop the CPU (the picture stays),
    // reset = reset and keep the current run state.
    void power_on();
    void power_off();
    void reset();
    bool powered() const;
    // Settings. The speed takes effect at once; CRT settings apply to the
    // next frontend_options() (a front end re-reads them when it wants).
    void set_speed_percent(unsigned pct);
    void set_crt(const CrtParams& crt);
    // The current settings (start-up configuration plus changes above).
    const AppConfig& config() const { return cfg_; }
    // Window options for the configured model and display.
    FrontendOptions frontend_options() const;

    int exit_code() const { return exit_code_; }
    // Why start() returned false (also written to stderr), for front ends
    // without a console; empty for one-shot actions such as --help.
    const std::string& last_error() const { return last_error_; }
    Emulator& emulator() { return *emu_; }
    const Model& model() const { return *model_; }

private:
    bool configure(int argc, char** argv);
    bool build_machine();
    bool fail(const std::string& message, int code = 2);
    int final_status() const;

    AppConfig cfg_;
    const Model* model_ = nullptr;
    std::unique_ptr<Emulator> emu_;
    std::unique_ptr<HostLink> host_link_;
    // Declared after the emulator so they are destroyed first.
    std::unique_ptr<Frontend> frontend_;
    std::unique_ptr<AppTooling> tooling_;
    bool background_ = false;
    bool finished_ = false;
    int exit_code_ = 0;
    std::string last_error_;
};

} // namespace tn34xx
