// SPDX-License-Identifier: GPL-3.0-or-later
#include "devtools/dev_tools.hpp"

#include "app/host_link.hpp"
#include "devtools/console.hpp"
#include "gdb/gdb_stub.hpp"
#include "machine/emulator.hpp"
#include "monitor/monitor.hpp"

#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <io.h>
#define tn_isatty _isatty
#define tn_fileno _fileno
#else
#include <unistd.h>
#define tn_isatty isatty
#define tn_fileno fileno
#endif

namespace tn34xx {

namespace {

bool stdin_is_tty() { return tn_isatty(tn_fileno(stdin)) != 0; }

class DevTools : public AppTooling {
public:
    bool start(Application& app, int& exit_code) override {
        const AppConfig& cfg = app.config();
        Emulator& emu = app.emulator();
        emu_ = &emu;
        history_file_ = cfg.history_file;
        monitor_ = std::make_unique<Monitor>(emu);
        monitor_->set_host_link(&app.host_link());
        monitor_->set_crt_params(app.frontend_options().crt);

        if (cfg.gdb) {
            std::string error;
            gdb_ = std::make_unique<GdbStub>(emu, monitor_.get());
            if (!gdb_->start(cfg.gdb_port, error)) {
                std::fprintf(stderr, "tn34xx: gdb: %s\n", error.c_str());
                exit_code = 2;
                return false;
            }
        }

        // Start-up script.
        if (!cfg.rc_file.empty()) {
            const bool ok = monitor_->run_script(cfg.rc_file);
            if (!ok && cfg.batch) {
                exit_code = 2;
                return false;
            }
        }
        if (cfg.batch || monitor_->quit_requested() || emu.quit_requested()) {
            exit_code = emu.quit_requested() ? emu.exit_code() : exit_status();
            return false;
        }

        if (cfg.gdb_wait) gdb_->wait_for_client();
        else if (!cfg.start_stopped && cfg.rc_file.empty()) emu.resume();
        return true;
    }

    void on_stop(const StopEvent& ev) override {
        if (monitor_) monitor_->notify_stop(ev);
    }

    void run_headless(Application&) override {
        if (stdin_is_tty()) {
            make_terminal_console(*monitor_, *emu_, history_file_)->run();
        } else {
            // Commands from a pipe: execute line by line.
            char buf[4096];
            while (!monitor_->quit_requested() && !emu_->quit_requested() && std::fgets(buf, sizeof buf, stdin))
                monitor_->execute(buf);
        }
    }

    void start_background(Application&) override {
        if (!monitor_ || !stdin_is_tty() || console_thread_.joinable()) return;
        console_ = make_terminal_console(*monitor_, *emu_, history_file_);
        Console* c = console_.get();
        console_thread_ = std::thread([c] { c->run(); });
    }

    void stop_background() override {
        if (!console_thread_.joinable()) return;
        console_->interrupt();
        console_thread_.join();
    }

    void shutdown() override {
        stop_background();
        if (gdb_) gdb_->stop();
    }

    int exit_status() const override { return monitor_ && monitor_->failures() ? 1 : 0; }

private:
    Emulator* emu_ = nullptr;
    std::string history_file_;
    std::unique_ptr<Monitor> monitor_;
    std::unique_ptr<GdbStub> gdb_;
    std::unique_ptr<Console> console_;
    std::thread console_thread_;
};

} // namespace

std::unique_ptr<AppTooling> make_dev_tools() { return std::make_unique<DevTools>(); }

} // namespace tn34xx
