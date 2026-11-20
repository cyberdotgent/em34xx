#include "monitor/monitor.hpp"

#include "bus/probe_device.hpp"
#include "cpu/i86_disasm.hpp"
#include "frontend/crt_render.hpp"
#include "machine/emulator.hpp"
#include "models/model.hpp"
#include "monitor/expr.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <replxx.hxx>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fstream>

namespace tn34xx {

namespace {

std::atomic<Emulator*> g_sigint_target{nullptr};

extern "C" void monitor_sigint(int) {
    if (Emulator* e = g_sigint_target.load()) e->request_stop();
}

// Installs a SIGINT handler that stops the machine for the lifetime of the
// object, so Ctrl-C interrupts a blocking "g" instead of killing the process.
class SigintScope {
public:
    explicit SigintScope(Emulator& emu) {
        g_sigint_target.store(&emu);
        prev_ = std::signal(SIGINT, monitor_sigint);
    }
    ~SigintScope() {
        std::signal(SIGINT, prev_);
        g_sigint_target.store(nullptr);
    }

private:
    void (*prev_)(int);
};

std::string flags_text(uint16_t f) {
    static const struct { uint16_t bit; char c; } k[] = {
        {FLAG_OF, 'o'}, {FLAG_DF, 'd'}, {FLAG_IF, 'i'}, {FLAG_TF, 't'}, {FLAG_SF, 's'},
        {FLAG_ZF, 'z'}, {FLAG_AF, 'a'}, {FLAG_PF, 'p'}, {FLAG_CF, 'c'},
    };
    std::string s;
    for (const auto& e : k) s += (f & e.bit) ? char(e.c - 32) : e.c;
    return s;
}

bool write_image(const std::string& path, const Framebuffer& fb, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { error = "cannot create " + path; return false; }
    const bool bmp = path.size() > 4 && to_lower(path.substr(path.size() - 4)) == ".bmp";
    if (bmp) {
        const uint32_t row = uint32_t((fb.width * 3 + 3) & ~3);
        const uint32_t size = 54 + row * uint32_t(fb.height);
        uint8_t h[54] = {'B', 'M'};
        auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) h[at + i] = uint8_t(v >> (8 * i)); };
        put32(2, size);
        put32(10, 54);
        put32(14, 40);
        put32(18, uint32_t(fb.width));
        put32(22, uint32_t(fb.height));
        h[26] = 1;
        h[28] = 24;
        put32(34, row * uint32_t(fb.height));
        std::fwrite(h, 1, sizeof h, f);
        std::vector<uint8_t> line(row, 0);
        for (int y = fb.height - 1; y >= 0; --y) {
            const uint32_t* p = fb.pixels.data() + size_t(y) * size_t(fb.width);
            for (int x = 0; x < fb.width; ++x) {
                line[size_t(x) * 3 + 0] = uint8_t(p[x]);
                line[size_t(x) * 3 + 1] = uint8_t(p[x] >> 8);
                line[size_t(x) * 3 + 2] = uint8_t(p[x] >> 16);
            }
            std::fwrite(line.data(), 1, line.size(), f);
        }
    } else {
        std::fprintf(f, "P6\n%d %d\n255\n", fb.width, fb.height);
        for (uint32_t p : fb.pixels) {
            const uint8_t rgb[3] = {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p)};
            std::fwrite(rgb, 1, 3, f);
        }
    }
    const bool ok = std::fclose(f) == 0;
    if (!ok) error = "error writing " + path;
    return ok;
}

} // namespace

struct Monitor::ReplxxHolder {
    replxx::Replxx rx;
};

Monitor::Monitor(Emulator& emu) : emu_(emu), rx_(std::make_unique<ReplxxHolder>()) {
    register_commands();
}

Monitor::~Monitor() = default;

// ---------------------------------------------------------------------------
// Infrastructure

void Monitor::out(const std::string& text) {
    if (sink_ && std::this_thread::get_id() == sink_thread_) {
        (*sink_)(text);
        return;
    }
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
}

bool Monitor::fail(const std::string& message) {
    out("error: " + message + "\n");
    return false;
}

const Monitor::Command* Monitor::find(const std::string& name) const {
    const std::string n = to_lower(name);
    for (const Command& c : commands_) {
        if (c.name == n) return &c;
        for (const std::string& a : c.aliases)
            if (a == n) return &c;
    }
    return nullptr;
}

std::string Monitor::join_args(const std::vector<std::string>& a, size_t from) const {
    std::string s;
    for (size_t i = from; i < a.size(); ++i) {
        if (i > from) s += ' ';
        s += a[i];
    }
    return s;
}

bool Monitor::execute(const std::string& raw) {
    std::lock_guard<std::recursive_mutex> lock(exec_mutex_);
    std::string line = trim(raw);
    if (line.empty() || line[0] == ';' || line[0] == '#') return true;
    std::vector<std::string> words = split_words(line);
    if (words.empty()) return true;
    const Command* c = find(words[0]);
    if (!c) return fail("unknown command '" + words[0] + "' (try 'help')");
    try {
        return c->fn(words);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

std::string Monitor::execute_captured(const std::string& line, bool& ok) {
    std::string buf;
    ok = execute_streamed(line, [&buf](const std::string& text) { buf += text; });
    return buf;
}

bool Monitor::execute_streamed(const std::string& line, const OutputSink& sink) {
    std::lock_guard<std::recursive_mutex> lock(exec_mutex_);
    const OutputSink* prev = sink_;
    const std::thread::id prev_thread = sink_thread_;
    sink_ = &sink;
    sink_thread_ = std::this_thread::get_id();
    bool ok;
    try {
        ok = execute(line);
    } catch (...) {
        sink_ = prev;
        sink_thread_ = prev_thread;
        throw;
    }
    sink_ = prev;
    sink_thread_ = prev_thread;
    return ok;
}

bool Monitor::run_script(const std::string& path) {
    std::ifstream in(path);
    if (!in) return fail("cannot open script '" + path + "'");
    if (++script_depth_ > 8) {
        --script_depth_;
        return fail("scripts nested too deeply");
    }
    std::string line;
    int lineno = 0;
    bool ok = true;
    while (!quit_ && std::getline(in, line)) {
        ++lineno;
        const std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        out(format("rc> %s\n", t.c_str()));
        if (!execute(t)) {
            out(format("%s:%d: command failed, script aborted\n", path.c_str(), lineno));
            ok = false;
            break;
        }
    }
    --script_depth_;
    return ok;
}

void Monitor::interactive(const std::string& history_file) {
    replxx::Replxx& rx = rx_->rx;
    rx.install_window_change_handler();
    rx.set_max_history_size(1000);
    if (!history_file.empty()) rx.history_load(history_file);
    rx.set_completion_callback([this](const std::string& input, int& context_len) {
        replxx::Replxx::completions_t out;
        if (input.find(' ') != std::string::npos) return out;
        context_len = int(input.size());
        for (const Command& c : commands_)
            if (starts_with(c.name, input)) out.emplace_back(c.name);
        return out;
    });
    log_set_sink([this](const std::string& line) { rx_->rx.print("%s\n", line.c_str()); });

    out("tn34xx monitor; 'help' lists commands, Ctrl-D or 'quit' exits.\n");
    while (!quit_ && !emu_.quit_requested()) {
        errno = 0;
        const char* input = rx.input(emu_.running() ? "tn34xx (running)> " : "tn34xx> ");
        if (!input) {
            if (errno == EAGAIN) continue;  // Ctrl-C at the prompt or interrupt_input()
            emu_.request_quit(int(emu_.exit_code()));
            break;
        }
        const std::string line = input;
        if (!trim(line).empty()) rx.history_add(line);
        execute(line);
    }
    log_set_sink(nullptr);
    if (!history_file.empty()) rx.history_save(history_file);
}

void Monitor::interrupt_input() {
    rx_->rx.emulate_key_press(replxx::Replxx::KEY::control('C'));
}

void Monitor::notify_stop(const StopEvent& ev) {
    if (waiting_) return;
    if (ev.reason == StopEvent::Reason::User) return;
    LOG_INFO("monitor", "machine stopped: %s at %06x%s%s", stop_reason_name(ev.reason), ev.pc,
             ev.detail.empty() ? "" : ": ", ev.detail.c_str());
}

// ---------------------------------------------------------------------------
// Helpers

bool Monitor::parse_addr(const std::string& text, uint32_t& outv) {
    std::string err;
    const bool ok = emu_.with_machine([&](Machine& m) { return eval_address(m, text, outv, err); });
    if (!ok) fail(err);
    return ok;
}

bool Monitor::parse_value(const std::string& text, int64_t& outv) {
    std::string err;
    const bool ok = emu_.with_machine([&](Machine& m) { return eval_expr(m, text, outv, err); });
    if (!ok) fail(err);
    return ok;
}

std::string Monitor::regs_text(Machine& m) {
    const CpuRegs& r = m.cpu().regs();
    return format("AX=%04X BX=%04X CX=%04X DX=%04X SP=%04X BP=%04X SI=%04X DI=%04X\n"
                  "DS=%04X ES=%04X SS=%04X CS=%04X IP=%04X FL=%04X %s%s\n",
                  r.gpr[AX], r.gpr[BX], r.gpr[CX], r.gpr[DX], r.gpr[SP], r.gpr[BP], r.gpr[SI], r.gpr[DI],
                  r.seg[DS], r.seg[ES], r.seg[SS], r.seg[CS], r.ip, r.flags, flags_text(r.flags).c_str(),
                  m.cpu().halted() ? " HALTED" : "");
}

std::string Monitor::location_line(Machine& m) {
    const CpuRegs& r = m.cpu().regs();
    Bus& bus = m.bus();
    DisasmResult d = disassemble([&](uint32_t a) { return bus.peek8(a); }, r.seg[CS], r.ip, m.cpu().level(),
                                 bus.address_mask());
    uint8_t bytes[16];
    for (unsigned i = 0; i < d.length && i < 16; ++i) bytes[i] = bus.peek8(m.cpu().linear(r.seg[CS], uint16_t(r.ip + i)));
    return format("%04X:%04X  %-20s %s\n", r.seg[CS], r.ip,
                  hex_bytes(bytes, std::min<unsigned>(d.length, 6)).c_str(), d.text.c_str());
}

void Monitor::print_stop(const StopEvent& ev) {
    std::string text;
    switch (ev.reason) {
    case StopEvent::Reason::Breakpoint: text = format("breakpoint %d hit\n", ev.id); break;
    case StopEvent::Reason::Watchpoint:
    case StopEvent::Reason::BusFault: text = format("%s: %s\n", stop_reason_name(ev.reason), ev.detail.c_str()); break;
    case StopEvent::Reason::User: text = "stopped\n"; break;
    default: break;
    }
    text += emu_.with_machine([&](Machine& m) { return regs_text(m) + location_line(m); });
    out(text);
}

bool Monitor::run_and_wait(uint64_t max_instructions) {
    SigintScope sig(emu_);
    waiting_ = true;
    emu_.resume(max_instructions);
    const StopEvent ev = emu_.wait_stopped();
    waiting_ = false;
    print_stop(ev);
    return true;
}

// ---------------------------------------------------------------------------
// Command table

void Monitor::register_commands() {
    using A = const std::vector<std::string>&;
    auto add = [&](std::string name, std::vector<std::string> aliases, std::string args, std::string help,
                   std::function<bool(A)> fn) {
        commands_.push_back(Command{std::move(name), std::move(aliases), std::move(args), std::move(help), std::move(fn)});
    };
    add("help", {"h", "?"}, "[command]", "list commands or describe one", [this](A a) { return cmd_help(a); });
    add("regs", {"r"}, "[reg=value ...]", "show registers, or assign them", [this](A a) { return cmd_regs(a); });
    add("step", {"s"}, "[count]", "execute instructions, then show state", [this](A a) { return cmd_step(a, false); });
    add("trace", {"t"}, "[count]", "execute instructions, printing each one", [this](A a) { return cmd_step(a, true); });
    add("proceed", {"p"}, "", "step over calls and software interrupts", [this](A a) { return cmd_proceed(a); });
    add("go", {"g", "c", "cont"}, "[addr]", "run until a stop (Ctrl-C), optionally to addr", [this](A a) { return cmd_go(a, true); });
    add("bg", {}, "[addr]", "run in the background and return to the prompt", [this](A a) { return cmd_go(a, false); });
    add("run", {}, "count", "run count instructions (no tracing)", [this](A a) { return cmd_run(a); });
    add("stop", {}, "", "stop a background run", [this](A a) { return cmd_stop(a); });
    add("bp", {"break"}, "addr", "set an execution breakpoint", [this](A a) { return cmd_break(a); });
    add("bc", {}, "id|addr|*", "clear breakpoint(s)", [this](A a) { return cmd_break_clear(a); });
    add("bl", {}, "", "list breakpoints and watchpoints", [this](A a) { return cmd_break_list(a); });
    add("wp", {"watch"}, "addr [len] [r|w|rw]", "set a memory watchpoint (default: write)", [this](A a) { return cmd_watch(a); });
    add("wc", {}, "id|*", "clear watchpoint(s)", [this](A a) { return cmd_watch_clear(a); });
    add("dump", {"d", "db"}, "[addr] [len]", "hex dump memory (no side effects)", [this](A a) { return cmd_dump(a); });
    add("unasm", {"u"}, "[addr] [count]", "disassemble", [this](A a) { return cmd_unassemble(a); });
    add("enter", {"e"}, "addr bytes...|\"text\"", "write bytes to RAM", [this](A a) { return cmd_enter(a); });
    add("fill", {"f"}, "addr len byte", "fill RAM", [this](A a) { return cmd_fill(a); });
    add("savemem", {}, "file addr len", "save memory to a file", [this](A a) { return cmd_savemem(a); });
    add("map", {}, "[ram|probe addr len [latch|value]]", "show or extend the memory map", [this](A a) { return cmd_map(a); });
    add("unmap", {}, "addr len", "remove mappings overlapping a range", [this](A a) { return cmd_unmap(a); });
    add("io", {}, "[probe port count [latch|value]]", "show or extend the I/O map", [this](A a) { return cmd_io(a); });
    add("dev", {}, "[name]", "list devices or describe one", [this](A a) { return cmd_dev(a); });
    add("unmapped", {}, "[clear]", "report unexpected accesses seen so far", [this](A a) { return cmd_unmapped(a); });
    add("policy", {}, "[stop|log|ignore]", "unexpected-access policy", [this](A a) { return cmd_policy(a); });
    add("log", {}, "spec", "set log levels, e.g. 'bus=debug,mmio=trace'", [this](A a) { return cmd_log(a); });
    add("reset", {}, "", "reset the machine", [this](A a) { return cmd_reset(a); });
    add("info", {"i"}, "", "model, ROMs and execution counters", [this](A a) { return cmd_info(a); });
    add("speed", {}, "[percent|max]", "show or set emulation speed", [this](A a) { return cmd_speed(a); });
    add("screenshot", {}, "file.bmp|file.ppm [raw]", "save the display (aspect-corrected unless raw)", [this](A a) { return cmd_screenshot(a); });
    add("twinax", {}, "status|send f/a ...|recv", "inspect or drive the Twinax line", [this](A a) { return cmd_twinax(a); });
    add("key", {}, "[+|-]name ...", "press and release host keys (HID names, e.g. enter f3 a 0x28)", [this](A a) { return cmd_key(a); });
    add("eeprom", {"nvram"}, "[dump|save [file]]", "show, dump or save the setup EEPROM", [this](A a) { return cmd_eeprom(a); });
    add("assert", {}, "expr [message]", "fail the script if expr is zero", [this](A a) { return cmd_assert(a); });
    add("echo", {}, "text", "print text", [this](A a) { return cmd_echo(a); });
    add("source", {}, "file", "run commands from a file", [this](A a) { return cmd_source(a); });
    add("quit", {"q", "exit"}, "[code]", "exit the emulator", [this](A a) { return cmd_quit(a); });
}

// ---------------------------------------------------------------------------
// Commands

bool Monitor::cmd_help(const std::vector<std::string>& a) {
    if (a.size() > 1) {
        const Command* c = find(a[1]);
        if (!c) return fail("unknown command '" + a[1] + "'");
        std::string s = c->name + " " + c->args + "\n  " + c->help + "\n";
        if (!c->aliases.empty()) {
            s += "  aliases:";
            for (const auto& al : c->aliases) s += " " + al;
            s += "\n";
        }
        out(s);
        return true;
    }
    std::string s;
    for (const Command& c : commands_) s += format("  %-11s %-34s %s\n", c.name.c_str(), c.args.c_str(), c.help.c_str());
    s += "\nNumbers are hex by default (#123 is decimal). Addresses are linear or seg:off;\n"
         "registers, byte[addr] and word[addr] can be used in expressions.\n";
    out(s);
    return true;
}

bool Monitor::cmd_regs(const std::vector<std::string>& a) {
    if (emu_.running() && a.size() > 1) return fail("stop the machine before changing registers");
    for (size_t i = 1; i < a.size(); ++i) {
        std::string name, value;
        const auto eq = a[i].find('=');
        if (eq != std::string::npos) {
            name = a[i].substr(0, eq);
            value = a[i].substr(eq + 1);
        } else if (i + 1 < a.size()) {
            name = a[i];
            value = a[++i];
        } else {
            return fail("expected reg=value");
        }
        int64_t v;
        if (!parse_value(value, v)) return false;
        const bool ok = emu_.with_machine([&](Machine& m) { return set_register(m, name, uint16_t(v)); });
        if (!ok) return fail("unknown register '" + name + "'");
    }
    out(emu_.with_machine([&](Machine& m) { return regs_text(m) + location_line(m); }));
    return true;
}

bool Monitor::cmd_step(const std::vector<std::string>& a, bool trace) {
    uint64_t count = 1;
    if (a.size() > 1) {
        int64_t v;
        if (!parse_value(a[1], v) || v <= 0) return v <= 0 ? fail("count must be positive") : false;
        count = uint64_t(v);
    }
    if (emu_.running()) {
        emu_.request_stop();
        emu_.wait_stopped();
    }
    if (trace) {
        emu_.with_machine([&](Machine& m) {
            m.trace_hook = [this](Machine& mm) {
                const CpuRegs& r = mm.cpu().regs();
                out(format("AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X SP=%04X  ", r.gpr[AX], r.gpr[BX],
                           r.gpr[CX], r.gpr[DX], r.gpr[SI], r.gpr[DI], r.gpr[SP]) +
                    location_line(mm));
            };
        });
    }
    const StopEvent ev = emu_.step(count);
    if (trace) emu_.with_machine([](Machine& m) { m.trace_hook = nullptr; });
    if (ev.reason != StopEvent::Reason::StepDone) {
        print_stop(ev);
    } else {
        out(emu_.with_machine([&](Machine& m) { return regs_text(m) + location_line(m); }));
    }
    return true;
}

bool Monitor::cmd_proceed(const std::vector<std::string>&) {
    if (emu_.running()) return fail("machine is running");
    bool is_call = false;
    uint32_t next = 0;
    emu_.with_machine([&](Machine& m) {
        const CpuRegs& r = m.cpu().regs();
        DisasmResult d = disassemble([&](uint32_t x) { return m.bus().peek8(x); }, r.seg[CS], r.ip,
                                     m.cpu().level(), m.bus().address_mask());
        is_call = d.is_call;
        next = m.cpu().linear(r.seg[CS], uint16_t(r.ip + d.length));
        if (is_call) m.add_breakpoint(next, true);
    });
    if (!is_call) return cmd_step({"step"}, false);
    return run_and_wait(UINT64_MAX);
}

bool Monitor::cmd_go(const std::vector<std::string>& a, bool wait) {
    if (emu_.running()) return fail("machine is already running");
    if (a.size() > 1) {
        uint32_t addr;
        if (!parse_addr(a[1], addr)) return false;
        emu_.with_machine([&](Machine& m) { m.add_breakpoint(addr, true); });
    }
    if (!wait) {
        emu_.resume();
        return true;
    }
    return run_and_wait(UINT64_MAX);
}

bool Monitor::cmd_run(const std::vector<std::string>& a) {
    if (a.size() < 2) return fail("usage: run count");
    int64_t v;
    if (!parse_value(a[1], v)) return false;
    if (v <= 0) return fail("count must be positive");
    if (emu_.running()) return fail("machine is already running");
    return run_and_wait(uint64_t(v));
}

bool Monitor::cmd_stop(const std::vector<std::string>&) {
    if (!emu_.running()) {
        out("not running\n");
        return true;
    }
    waiting_ = true;
    emu_.request_stop();
    const StopEvent ev = emu_.wait_stopped();
    waiting_ = false;
    print_stop(ev);
    return true;
}

bool Monitor::cmd_break(const std::vector<std::string>& a) {
    if (a.size() < 2) return cmd_break_list(a);
    uint32_t addr;
    if (!parse_addr(join_args(a, 1), addr)) return false;
    const int id = emu_.with_machine([&](Machine& m) { return m.add_breakpoint(addr); });
    out(format("breakpoint %d at %06X\n", id, addr));
    return true;
}

bool Monitor::cmd_break_clear(const std::vector<std::string>& a) {
    if (a.size() < 2) return fail("usage: bc id|addr|*");
    if (a[1] == "*") {
        emu_.with_machine([](Machine& m) { m.clear_breakpoints(); });
        return true;
    }
    uint64_t id;
    if (parse_uint(a[1], id, 10) && a[1].find(':') == std::string::npos) {
        if (emu_.with_machine([&](Machine& m) { return m.remove_breakpoint(int(id)); })) return true;
    }
    uint32_t addr;
    if (!parse_addr(a[1], addr)) return false;
    if (!emu_.with_machine([&](Machine& m) { return m.remove_breakpoint_at(addr); }))
        return fail("no such breakpoint");
    return true;
}

bool Monitor::cmd_break_list(const std::vector<std::string>&) {
    std::string s = emu_.with_machine([&](Machine& m) {
        std::string t;
        for (const Breakpoint& b : m.breakpoints())
            t += format("  bp %-3d %06X%s\n", b.id, b.address, b.temporary ? " (temporary)" : "");
        for (const Watchpoint& w : m.bus().watchpoints()) {
            const char* k = w.kind == AccessKind::Read ? "r" : w.kind == AccessKind::Write ? "w" : "rw";
            t += format("  wp %-3d %06X len %X %s\n", w.id, w.start, w.length, k);
        }
        return t;
    });
    out(s.empty() ? "no breakpoints or watchpoints\n" : s);
    return true;
}

bool Monitor::cmd_watch(const std::vector<std::string>& a) {
    if (a.size() < 2) return fail("usage: wp addr [len] [r|w|rw]");
    uint32_t addr;
    if (!parse_addr(a[1], addr)) return false;
    uint32_t len = 1;
    AccessKind kind = AccessKind::Write;
    for (size_t i = 2; i < a.size(); ++i) {
        const std::string k = to_lower(a[i]);
        if (k == "r") kind = AccessKind::Read;
        else if (k == "w") kind = AccessKind::Write;
        else if (k == "rw" || k == "a") kind = AccessKind::ReadWrite;
        else {
            int64_t v;
            if (!parse_value(a[i], v) || v <= 0) return fail("bad length '" + a[i] + "'");
            len = uint32_t(v);
        }
    }
    const int id = emu_.with_machine([&](Machine& m) { return m.bus().add_watchpoint(addr, len, kind); });
    out(format("watchpoint %d at %06X len %X\n", id, addr, len));
    return true;
}

bool Monitor::cmd_watch_clear(const std::vector<std::string>& a) {
    if (a.size() < 2) return fail("usage: wc id|*");
    if (a[1] == "*") {
        emu_.with_machine([](Machine& m) { m.bus().clear_watchpoints(); });
        return true;
    }
    uint64_t id;
    if (!parse_uint(a[1], id, 10)) return fail("bad watchpoint id");
    if (!emu_.with_machine([&](Machine& m) { return m.bus().remove_watchpoint(int(id)); }))
        return fail("no such watchpoint");
    return true;
}

bool Monitor::cmd_dump(const std::vector<std::string>& a) {
    uint32_t addr = have_next_dump_ ? next_dump_ : 0;
    uint32_t len = 0x80;
    if (a.size() > 1 && !parse_addr(a[1], addr)) return false;
    if (a.size() > 2) {
        int64_t v;
        if (!parse_value(a[2], v) || v <= 0) return fail("bad length");
        len = uint32_t(v);
    }
    std::string s = emu_.with_machine([&](Machine& m) {
        std::string t;
        const uint32_t mask = m.bus().address_mask();
        for (uint32_t row = addr & ~0xFu; row < addr + len; row += 16) {
            t += format("%06X  ", row & mask);
            std::string ascii;
            for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t x = row + i;
                if (x < addr || x >= addr + len) {
                    t += "   ";
                    ascii += ' ';
                    continue;
                }
                const uint8_t b = m.bus().peek8(x & mask);
                t += format("%02X%c", b, i == 7 ? '-' : ' ');
                ascii += (b >= 0x20 && b < 0x7F) ? char(b) : '.';
            }
            t += " " + ascii + "\n";
        }
        return t;
    });
    out(s);
    next_dump_ = addr + len;
    have_next_dump_ = true;
    return true;
}

bool Monitor::cmd_unassemble(const std::vector<std::string>& a) {
    unsigned count = 16;
    uint16_t seg = 0, off = 0;
    bool have = false;
    if (a.size() > 1) {
        const auto colon = a[1].find(':');
        int64_t s = 0, o = 0;
        if (colon != std::string::npos) {
            if (!parse_value(a[1].substr(0, colon), s) || !parse_value(a[1].substr(colon + 1), o)) return false;
            seg = uint16_t(s);
            off = uint16_t(o);
        } else {
            uint32_t lin;
            if (!parse_addr(a[1], lin)) return false;
            seg = uint16_t((lin >> 4) & 0xF000);
            off = uint16_t(lin - (uint32_t(seg) << 4));
        }
        have = true;
    }
    if (a.size() > 2) {
        int64_t v;
        if (!parse_value(a[2], v) || v <= 0) return fail("bad count");
        count = unsigned(v);
    }
    std::string s = emu_.with_machine([&](Machine& m) {
        if (!have) {
            if (have_next_unasm_) {
                seg = uint16_t(next_unasm_ >> 16);
                off = uint16_t(next_unasm_);
            } else {
                seg = m.cpu().regs().seg[CS];
                off = m.cpu().regs().ip;
            }
        }
        std::string t;
        Bus& bus = m.bus();
        for (unsigned i = 0; i < count; ++i) {
            DisasmResult d = disassemble([&](uint32_t x) { return bus.peek8(x); }, seg, off, m.cpu().level(),
                                         bus.address_mask());
            uint8_t bytes[16];
            for (unsigned j = 0; j < d.length && j < 16; ++j) bytes[j] = bus.peek8(m.cpu().linear(seg, uint16_t(off + j)));
            t += format("%04X:%04X  %-20s %s\n", seg, off, hex_bytes(bytes, std::min<unsigned>(d.length, 6)).c_str(),
                        d.text.c_str());
            off = uint16_t(off + d.length);
        }
        next_unasm_ = (uint32_t(seg) << 16) | off;
        return t;
    });
    have_next_unasm_ = true;
    out(s);
    return true;
}

bool Monitor::cmd_enter(const std::vector<std::string>& a) {
    if (a.size() < 3) return fail("usage: e addr bytes...");
    uint32_t addr;
    if (!parse_addr(a[1], addr)) return false;
    std::vector<uint8_t> bytes;
    for (size_t i = 2; i < a.size(); ++i) {
        int64_t v;
        if (parse_value(a[i], v) && v >= 0 && v <= 0xFF) {
            bytes.push_back(uint8_t(v));
        } else {
            // Not a number: treat the word as literal text.
            for (char ch : a[i]) bytes.push_back(uint8_t(ch));
        }
    }
    bool ok = emu_.with_machine([&](Machine& m) {
        for (size_t i = 0; i < bytes.size(); ++i)
            if (!m.bus().poke8(addr + uint32_t(i), bytes[i])) return false;
        return true;
    });
    if (!ok) return fail("address range is not RAM (ROM and devices cannot be written from the monitor)");
    return true;
}

bool Monitor::cmd_fill(const std::vector<std::string>& a) {
    if (a.size() < 4) return fail("usage: f addr len byte");
    uint32_t addr;
    int64_t len, val;
    if (!parse_addr(a[1], addr) || !parse_value(a[2], len) || !parse_value(a[3], val)) return false;
    bool ok = emu_.with_machine([&](Machine& m) {
        for (int64_t i = 0; i < len; ++i)
            if (!m.bus().poke8(addr + uint32_t(i), uint8_t(val))) return false;
        return true;
    });
    if (!ok) return fail("address range is not RAM");
    return true;
}

bool Monitor::cmd_savemem(const std::vector<std::string>& a) {
    if (a.size() < 4) return fail("usage: savemem file addr len");
    uint32_t addr;
    int64_t len;
    if (!parse_addr(a[2], addr) || !parse_value(a[3], len) || len <= 0) return false;
    std::vector<uint8_t> buf(size_t(len), 0);
    emu_.with_machine([&](Machine& m) {
        for (int64_t i = 0; i < len; ++i) buf[size_t(i)] = m.bus().peek8(addr + uint32_t(i));
    });
    FILE* f = std::fopen(a[1].c_str(), "wb");
    if (!f) return fail("cannot create " + a[1]);
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    out(format("saved %zu bytes from %06X to %s\n", buf.size(), addr, a[1].c_str()));
    return true;
}

bool Monitor::cmd_map(const std::vector<std::string>& a) {
    if (a.size() == 1) {
        out(emu_.with_machine([](Machine& m) {
            std::vector<const Region*> rs;
            for (const auto& r : m.bus().regions()) rs.push_back(r.get());
            std::sort(rs.begin(), rs.end(), [](const Region* x, const Region* y) { return x->base < y->base; });
            std::string t;
            for (const Region* r : rs) {
                const char* k = r->kind == Region::Kind::Ram ? "ram" : r->kind == Region::Kind::Rom ? "rom" : "mmio";
                t += format("  %06X-%06X  %-4s  %s\n", r->base, r->base + r->size - 1, k, r->name.c_str());
            }
            t += format("  unexpected-access policy: %s\n", unmapped_policy_name(m.bus().policy()));
            return t;
        }));
        return true;
    }
    if (a.size() < 4) return fail("usage: map ram|probe addr len [latch|value]");
    uint32_t addr;
    int64_t len;
    if (!parse_addr(a[2], addr) || !parse_value(a[3], len) || len <= 0) return false;
    const std::string kind = to_lower(a[1]);
    if (kind == "ram") {
        emu_.with_machine([&](Machine& m) { m.bus().map_ram(format("ram@%06X", addr), addr, uint32_t(len)); });
    } else if (kind == "probe") {
        bool latch = true;
        uint8_t fixed = 0xFF;
        if (a.size() > 4 && to_lower(a[4]) != "latch") {
            int64_t v;
            if (!parse_value(a[4], v)) return false;
            latch = false;
            fixed = uint8_t(v);
        }
        emu_.with_machine([&](Machine& m) {
            const std::string name = format("probe@%06X", addr);
            ProbeDevice& p = m.add_device<ProbeDevice>(name, uint32_t(len), latch, fixed);
            m.bus().map_mmio(name, addr, uint32_t(len), &p);
        });
    } else {
        return fail("unknown mapping kind '" + a[1] + "'");
    }
    return true;
}

bool Monitor::cmd_unmap(const std::vector<std::string>& a) {
    if (a.size() < 3) return fail("usage: unmap addr len");
    uint32_t addr;
    int64_t len;
    if (!parse_addr(a[1], addr) || !parse_value(a[2], len) || len <= 0) return false;
    emu_.with_machine([&](Machine& m) { m.bus().unmap(addr, uint32_t(len)); });
    return true;
}

bool Monitor::cmd_io(const std::vector<std::string>& a) {
    if (a.size() == 1) {
        out(emu_.with_machine([](Machine& m) {
            std::string t;
            for (const IoMapping& io : m.bus().io_mappings())
                t += format("  %04X-%04X  %s\n", io.base, unsigned(io.base + io.count - 1), io.name.c_str());
            return t.empty() ? std::string("  no I/O ports mapped\n") : t;
        }));
        return true;
    }
    if (a.size() < 4 || to_lower(a[1]) != "probe") return fail("usage: io probe port count [latch|value]");
    int64_t port, count;
    if (!parse_value(a[2], port) || !parse_value(a[3], count) || count <= 0) return false;
    bool latch = true;
    uint8_t fixed = 0xFF;
    if (a.size() > 4 && to_lower(a[4]) != "latch") {
        int64_t v;
        if (!parse_value(a[4], v)) return false;
        latch = false;
        fixed = uint8_t(v);
    }
    emu_.with_machine([&](Machine& m) {
        const std::string name = format("ioprobe@%04X", unsigned(port));
        ProbeDevice& p = m.add_device<ProbeDevice>(name, 0x10000, latch, fixed);
        m.bus().map_io(name, uint16_t(port), uint32_t(count), &p);
    });
    return true;
}

bool Monitor::cmd_dev(const std::vector<std::string>& a) {
    std::string s = emu_.with_machine([&](Machine& m) -> std::string {
        if (a.size() > 1) {
            Device* d = m.find_device(a[1]);
            if (!d) return {};
            std::string t = d->describe();
            return t.empty() ? std::string("(no state)\n") : t;
        }
        std::string t;
        for (const auto& d : m.devices()) t += "  " + d->name() + "\n";
        return t.empty() ? std::string("  no devices\n") : t;
    });
    if (s.empty()) return fail("no device named '" + a[1] + "'");
    out(s);
    return true;
}

bool Monitor::cmd_unmapped(const std::vector<std::string>& a) {
    if (a.size() > 1 && to_lower(a[1]) == "clear") {
        emu_.with_machine([](Machine& m) { m.bus().clear_unmapped_log(); });
        return true;
    }
    out(emu_.with_machine([](Machine& m) {
        std::string t;
        for (const auto& kv : m.bus().unmapped_log()) {
            const bool io = (kv.first >> 40) & 1;
            const bool wr = (kv.first >> 39) & 1;
            const uint32_t addr = uint32_t(kv.first & 0xFFFFFFFF);
            t += format("  %-3s %-5s %06X  x%-8llu first pc %06X  last %02X\n", io ? "io" : "mem",
                        wr ? "write" : "read", addr, (unsigned long long)kv.second.count, kv.second.first_pc,
                        kv.second.last_value);
        }
        return t.empty() ? std::string("  no unexpected accesses\n") : t;
    }));
    return true;
}

bool Monitor::cmd_policy(const std::vector<std::string>& a) {
    if (a.size() > 1) {
        UnmappedPolicy p;
        if (!parse_unmapped_policy(a[1], p)) return fail("policy must be stop, log or ignore");
        emu_.with_machine([&](Machine& m) { m.bus().set_policy(p); });
    }
    out(format("unexpected-access policy: %s\n",
               emu_.with_machine([](Machine& m) { return unmapped_policy_name(m.bus().policy()); })));
    return true;
}

bool Monitor::cmd_log(const std::vector<std::string>& a) {
    if (a.size() < 2) return fail("usage: log spec (e.g. 'info' or 'bus=debug,mmio=trace')");
    std::string err;
    if (!log_configure(join_args(a, 1), err)) return fail(err);
    return true;
}

bool Monitor::cmd_reset(const std::vector<std::string>&) {
    if (emu_.running()) {
        emu_.request_stop();
        emu_.wait_stopped();
    }
    emu_.with_machine([](Machine& m) { m.reset(); });
    out(emu_.with_machine([&](Machine& m) { return regs_text(m) + location_line(m); }));
    return true;
}

bool Monitor::cmd_info(const std::vector<std::string>&) {
    out(emu_.with_machine([&](Machine& m) {
        const ModelInfo& mi = m.model();
        std::string t = format("model     %s (%s), status: %s\n", mi.name.c_str(), mi.description.c_str(),
                               model_status_name(mi.status));
        t += format("cpu       %s, %u-bit addresses, nominal %.3f MHz\n", cpu_level_name(mi.cpu), mi.address_bits,
                    double(mi.clock_hz) / 1e6);
        for (const RomSpec& r : mi.roms) t += format("rom       %06X  %-12s %s\n", r.base, r.file.c_str(), r.label.c_str());
        for (const std::string& n : mi.notes) t += "note      " + n + "\n";
        t += format("state     %s\n", emu_.running() ? "running" : "stopped");
        t += format("executed  %llu instructions, %llu cycles\n", (unsigned long long)m.cpu().instructions(),
                    (unsigned long long)m.cpu().cycles());
        t += format("speed     %s\n", emu_.speed_percent() ? format("%u%%", emu_.speed_percent()).c_str() : "unthrottled");
        t += m.twinax_host() ? m.twinax_host()->describe() : std::string();
        if (m.nvram()) t += "eeprom    " + m.nvram()->nv_status() + "\n";
        return t;
    }));
    return true;
}

bool Monitor::cmd_eeprom(const std::vector<std::string>& a) {
    const std::string sub = a.size() > 1 ? to_lower(a[1]) : std::string();
    std::string text, err;
    const bool ok = emu_.with_machine([&](Machine& m) {
        NonVolatileStore* nv = m.nvram();
        if (!nv) { err = "this model has no EEPROM"; return false; }
        if (sub.empty()) {
            text = nv->nv_status() + "\n";
        } else if (sub == "dump") {
            const std::vector<uint8_t>& d = nv->nv_contents();
            for (size_t i = 0; i < d.size(); i += 16) {
                text += format("%03zX:", i);
                for (size_t j = i; j < i + 16 && j < d.size(); ++j) text += format(" %02X", d[j]);
                text += "\n";
            }
        } else if (sub == "save") {
            if (!nv->nv_save(a.size() > 2 ? a[2] : std::string(), err)) return false;
            text = nv->nv_status() + "\n";
        } else {
            err = "usage: eeprom [dump|save [file]]";
            return false;
        }
        return true;
    });
    if (!ok) return fail(err);
    out(text);
    return true;
}

bool Monitor::cmd_speed(const std::vector<std::string>& a) {
    if (a.size() > 1) {
        uint64_t v = 0;
        if (to_lower(a[1]) != "max" && !parse_uint(a[1], v, 10)) return fail("speed is a decimal percentage or 'max'");
        emu_.set_speed_percent(unsigned(v));
    }
    out(emu_.speed_percent() ? format("speed %u%%\n", emu_.speed_percent()) : std::string("speed unthrottled\n"));
    return true;
}

bool Monitor::cmd_screenshot(const std::vector<std::string>& a) {
    if (a.size() < 2) return fail("usage: screenshot file.bmp|file.ppm [raw]");
    Framebuffer fb;
    emu_.with_machine([&](Machine& m) { m.render(fb); });
    const bool raw = a.size() > 2 && to_lower(a[2]) == "raw";
    if (!raw && (fb.presented_width() != fb.width || fb.presented_height() != fb.height)) {
        // Render the whole tube as the window shows it (CRT scan lines,
        // border, aspect), at a height of three output pixels per raster line.
        int out_w = 1, out_h = 1;
        const int target_h = int(fb.height * 3 / std::max(0.1, 1.0 - 2.0 * fb.border_y));
        crt_fit(fb, 1 << 16, target_h, out_w, out_h);
        Framebuffer out;
        out.resize(out_w, out_h);
        crt_render(fb, out_w, out_h, crt_params_, out.pixels);
        fb = std::move(out);
    }
    std::string err;
    if (!write_image(a[1], fb, err)) return fail(err);
    out(format("saved %dx%d screenshot to %s\n", fb.width, fb.height, a[1].c_str()));
    return true;
}

bool Monitor::cmd_twinax(const std::vector<std::string>& a) {
    const std::string sub = a.size() > 1 ? to_lower(a[1]) : "status";
    if (sub == "status") {
        out(emu_.with_machine([](Machine& m) { return m.twinax_host()->describe(); }));
        return true;
    }
    ScriptTwinaxHost* script = emu_.with_machine([](Machine& m) {
        return dynamic_cast<ScriptTwinaxHost*>(m.twinax_host());
    });
    if (!script) return fail("twinax send/recv need --twinax-host script");
    if (sub == "send") {
        // Frames are written data/address, e.g. "twinax send 01/0 00/7".
        TwinaxMessage msg;
        for (size_t i = 2; i < a.size(); ++i) {
            const auto slash = a[i].find('/');
            uint64_t d, addr = 0;
            if (!parse_uint(a[i].substr(0, slash), d, 16) || d > 0xFF) return fail("bad frame '" + a[i] + "'");
            if (slash != std::string::npos && (!parse_uint(a[i].substr(slash + 1), addr, 10) || addr > 7))
                return fail("bad station address in '" + a[i] + "'");
            msg.push_back(TwinaxFrame{uint8_t(d), uint8_t(addr)});
        }
        if (msg.empty()) return fail("usage: twinax send data/addr ...");
        script->queue(msg);
        out(format("queued %zu frame(s); no link hardware is modelled yet, so nothing consumes them\n", msg.size()));
        return true;
    }
    if (sub == "recv") {
        auto msgs = script->take_received();
        if (msgs.empty()) out("no messages from the station\n");
        for (const auto& msg : msgs) out("  " + format_twinax_message(msg) + "\n");
        return true;
    }
    return fail("usage: twinax status|send|recv");
}

namespace {

// Host key names for the key command, as USB HID usages (what the window
// front end sends).
bool parse_host_key(const std::string& name, uint32_t& usage) {
    const std::string n = to_lower(name);
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') { usage = 0x04 + uint32_t(n[0] - 'a'); return true; }
    if (n.size() == 1 && n[0] >= '1' && n[0] <= '9') { usage = 0x1E + uint32_t(n[0] - '1'); return true; }
    if (n == "0") { usage = 0x27; return true; }
    if (n.size() >= 2 && n[0] == 'f' && n[1] != 'f') {
        uint64_t f = 0;
        if (parse_uint(n.substr(1), f, 10) && f >= 1 && f <= 24) {
            usage = f <= 12 ? 0x3A + uint32_t(f - 1) : 0x68 + uint32_t(f - 13);
            return true;
        }
    }
    static const struct { const char* name; uint32_t usage; } names[] = {
        {"enter", 0x28}, {"return", 0x28}, {"esc", 0x29}, {"escape", 0x29}, {"backspace", 0x2A},
        {"tab", 0x2B}, {"space", 0x2C}, {"minus", 0x2D}, {"equals", 0x2E}, {"comma", 0x36},
        {"period", 0x37}, {"slash", 0x38}, {"capslock", 0x39}, {"insert", 0x49}, {"home", 0x4A},
        {"pageup", 0x4B}, {"delete", 0x4C}, {"end", 0x4D}, {"pagedown", 0x4E}, {"right", 0x4F},
        {"left", 0x50}, {"down", 0x51}, {"up", 0x52}, {"kpenter", 0x58}, {"lctrl", 0xE0},
        {"lshift", 0xE1}, {"lalt", 0xE2}, {"rctrl", 0xE4}, {"rshift", 0xE5}, {"ralt", 0xE6},
    };
    for (const auto& k : names) {
        if (n == k.name) { usage = k.usage; return true; }
    }
    uint64_t v = 0;
    if (n.size() > 2 && n[0] == '0' && n[1] == 'x' && parse_uint(n.substr(2), v, 16) && v <= 0xFFFF) {
        usage = uint32_t(v);
        return true;
    }
    return false;
}

} // namespace

bool Monitor::cmd_key(const std::vector<std::string>& a) {
    if (a.size() < 2) return fail("usage: key [+|-]name ...  (+ press only, - release only)");
    std::vector<HostKeyEvent> events;
    for (size_t i = 1; i < a.size(); ++i) {
        std::string name = a[i];
        int mode = 0;   // 0 press and release, 1 press, 2 release
        if (name.size() > 1 && (name[0] == '+' || name[0] == '-')) {
            mode = name[0] == '+' ? 1 : 2;
            name = name.substr(1);
        }
        uint32_t usage = 0;
        if (!parse_host_key(name, usage)) return fail("unknown key '" + name + "'");
        if (mode != 2) events.push_back(HostKeyEvent{usage, true, name});
        if (mode != 1) events.push_back(HostKeyEvent{usage, false, name});
    }
    emu_.with_machine([&](Machine& m) {
        for (const HostKeyEvent& ev : events) m.key_event(ev);
    });
    return true;
}

bool Monitor::cmd_assert(const std::vector<std::string>& a) {
    if (a.size() < 2) return fail("usage: assert expr [message]");
    // The expression is the first word; anything after it is the message.
    int64_t v;
    if (!parse_value(a[1], v)) return false;
    if (v == 0) {
        ++failures_;
        out(format("ASSERTION FAILED: %s%s%s\n", a[1].c_str(), a.size() > 2 ? " -- " : "", join_args(a, 2).c_str()));
    } else {
        out(format("ok: %s\n", a[1].c_str()));
    }
    return true;
}

bool Monitor::cmd_echo(const std::vector<std::string>& a) {
    out(join_args(a, 1) + "\n");
    return true;
}

bool Monitor::cmd_source(const std::vector<std::string>& a) {
    if (a.size() < 2) return fail("usage: source file");
    return run_script(a[1]);
}

bool Monitor::cmd_quit(const std::vector<std::string>& a) {
    int code = failures_ ? 1 : 0;
    if (a.size() > 1) {
        uint64_t v;
        if (!parse_uint(a[1], v, 10)) return fail("exit code must be a decimal number");
        code = int(v);
    }
    quit_ = true;
    emu_.request_quit(code);
    return true;
}

} // namespace tn34xx
