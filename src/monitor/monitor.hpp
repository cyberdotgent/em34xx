// SPDX-License-Identifier: GPL-3.0-or-later
// Interactive and scripted debug monitor.
//
// Commands can come from an interactive console (src/devtools/console.hpp), from
// an rc file (--rc), from stdin, or from GDB's "monitor" command. The
// monitor itself has no UI: it executes lines and writes their output. Scripts stop at the first command
// error; failed assertions are counted and turn into a non-zero exit code.
#pragma once

#include "present/crt_render.hpp"
#include "host/twinax_link.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tn34xx {

class Emulator;
class HostLink;
class Machine;
struct StopEvent;

class Monitor {
public:
    explicit Monitor(Emulator& emu);
    ~Monitor();

    // Execute one command line. Returns false on error.
    bool execute(const std::string& line);

    // Execute a command and return its output instead of printing it.
    std::string execute_captured(const std::string& line, bool& ok);

    // Execute a command, handing its output to `sink` as it is produced
    // (possibly in many small pieces) instead of printing it. Output from
    // other threads while the command runs still goes to stdout.
    using OutputSink = std::function<void(const std::string&)>;
    bool execute_streamed(const std::string& line, const OutputSink& sink);

    // Run a script file. Returns false if a command failed or the script
    // could not be read. Stops early when "quit" is executed.
    bool run_script(const std::string& path);

    // Command names, for completion in an interactive console.
    std::vector<std::string> command_names() const;

    // `connect` and `disconnect` drive the application's host link; without
    // one they report that the bridge is not available.
    void set_host_link(HostLink* link) { host_link_ = link; }

    // CRT presentation used by "screenshot" so it matches the window.
    void set_crt_params(const CrtParams& p) { crt_params_ = p; }

    unsigned failures() const { return failures_; }
    bool quit_requested() const { return quit_; }

    // Called (from the emulation thread) whenever the machine stops on its
    // own; prints a notice unless a command is waiting for that stop.
    void notify_stop(const StopEvent& ev);

private:
    struct Command {
        std::string name;
        std::vector<std::string> aliases;
        std::string args;
        std::string help;
        std::function<bool(const std::vector<std::string>&)> fn;
    };

    void register_commands();
    const Command* find(const std::string& name) const;
    void out(const std::string& text);
    bool fail(const std::string& message);

    // Blocking run with Ctrl-C support; prints the stop location.
    bool run_and_wait(uint64_t max_instructions);
    void print_stop(const StopEvent& ev);
    std::string location_line(Machine& m);
    std::string regs_text(Machine& m);
    bool parse_addr(const std::string& text, uint32_t& out);
    bool parse_value(const std::string& text, int64_t& out);
    std::string join_args(const std::vector<std::string>& a, size_t from) const;

    // Command implementations.
    bool cmd_help(const std::vector<std::string>& a);
    bool cmd_regs(const std::vector<std::string>& a);
    bool cmd_step(const std::vector<std::string>& a, bool trace);
    bool cmd_proceed(const std::vector<std::string>& a);
    bool cmd_go(const std::vector<std::string>& a, bool wait);
    bool cmd_run(const std::vector<std::string>& a);
    bool cmd_stop(const std::vector<std::string>& a);
    bool cmd_break(const std::vector<std::string>& a);
    bool cmd_break_clear(const std::vector<std::string>& a);
    bool cmd_break_list(const std::vector<std::string>& a);
    bool cmd_watch(const std::vector<std::string>& a);
    bool cmd_watch_clear(const std::vector<std::string>& a);
    bool cmd_dump(const std::vector<std::string>& a);
    bool cmd_unassemble(const std::vector<std::string>& a);
    bool cmd_enter(const std::vector<std::string>& a);
    bool cmd_fill(const std::vector<std::string>& a);
    bool cmd_savemem(const std::vector<std::string>& a);
    bool cmd_map(const std::vector<std::string>& a);
    bool cmd_unmap(const std::vector<std::string>& a);
    bool cmd_io(const std::vector<std::string>& a);
    bool cmd_dev(const std::vector<std::string>& a);
    bool cmd_unmapped(const std::vector<std::string>& a);
    bool cmd_policy(const std::vector<std::string>& a);
    bool cmd_log(const std::vector<std::string>& a);
    bool cmd_reset(const std::vector<std::string>& a);
    bool cmd_info(const std::vector<std::string>& a);
    bool cmd_eeprom(const std::vector<std::string>& a);
    bool cmd_speed(const std::vector<std::string>& a);
    bool cmd_screenshot(const std::vector<std::string>& a);
    bool cmd_twinax(const std::vector<std::string>& a);
    bool cmd_connect(const std::vector<std::string>& a);
    bool cmd_disconnect(const std::vector<std::string>& a);
    bool cmd_key(const std::vector<std::string>& a);
    bool cmd_assert(const std::vector<std::string>& a);
    bool cmd_echo(const std::vector<std::string>& a);
    bool cmd_source(const std::vector<std::string>& a);
    bool cmd_quit(const std::vector<std::string>& a);

    Emulator& emu_;
    HostLink* host_link_ = nullptr;
    std::recursive_mutex exec_mutex_;  // monitor prompt vs. gdb "monitor"
    std::vector<Command> commands_;
    const OutputSink* sink_ = nullptr;
    std::thread::id sink_thread_;
    bool waiting_ = false;          // a command is blocked waiting for a stop
    bool quit_ = false;
    unsigned failures_ = 0;
    int script_depth_ = 0;
    uint32_t next_dump_ = 0;
    uint32_t next_unasm_ = 0;
    bool have_next_dump_ = false;
    bool have_next_unasm_ = false;

    CrtParams crt_params_;
};

} // namespace tn34xx
