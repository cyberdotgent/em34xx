#include "app/application.hpp"

#include "gdb/gdb_stub.hpp"
#include "host/wsc/tn5250_bridge.hpp"
#include "machine/emulator.hpp"
#include "models/model.hpp"
#include "monitor/monitor.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <cstdio>
#include <cstring>
#include <exception>

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

const char* kVersion = "0.0.0";

bool stdin_is_tty() { return tn_isatty(tn_fileno(stdin)) != 0; }

void list_models() {
    std::printf("%-6s %-16s %-6s %s\n", "model", "status", "cpu", "description");
    for (const Model* m : all_models()) {
        const ModelInfo& i = m->info();
        std::printf("%-6s %-16s %-6s %s\n", i.name.c_str(), model_status_name(i.status), cpu_level_name(i.cpu),
                    i.description.c_str());
    }
}

int check_roms(const Model& model, const AppConfig& cfg) {
    const ModelInfo& info = model.info();
    std::vector<LoadedRom> roms;
    std::vector<std::string> warnings;
    std::string error;
    const bool ok = load_roms(cfg.rom_dir, info.roms, info.known_bad_roms, true, roms, warnings, error);
    if (!ok) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    int bad = 0;
    for (const LoadedRom& r : roms) {
        std::printf("%-12s %06X  %-9s %s\n", r.spec.file.c_str(), r.spec.base, rom_verdict_name(r.verdict),
                    r.sha256.c_str());
        if (!r.note.empty()) std::printf("             %s\n", r.note.c_str());
        if (r.verdict == LoadedRom::Verdict::KnownBad) ++bad;
    }
    return bad ? 1 : 0;
}

} // namespace

Application::Application(FrontendRegistry frontends, ConsoleFactory console)
    : frontends_(std::move(frontends)), console_factory_(std::move(console)) {}

Application::~Application() { finish(); }

int Application::main(int argc, char** argv) {
    if (!start(argc, argv)) {
        finish();
        return exit_code_;
    }
    return run();
}

bool Application::fail(const std::string& message, int code) {
    std::fprintf(stderr, "tn34xx: %s\n", message.c_str());
    exit_code_ = code;
    return false;
}

// --- start-up ---------------------------------------------------------------

bool Application::start(int argc, char** argv) {
    exit_code_ = 0;
    if (!configure(argc, argv)) return false;
    if (!build_machine()) return false;
    return start_debuggers();
}

// Configuration and one-shot actions. Returns false to end the program.
bool Application::configure(int argc, char** argv) {
    std::string error;
    if (!parse_command_line(argc, argv, cfg_, error)) return fail(error);
    if (cfg_.show_help) {
        std::fputs(usage_text(argv[0]).c_str(), stdout);
        return false;
    }
    if (cfg_.show_version) {
        std::printf("tn34xx %s\n", kVersion);
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--dump-config")) {
            std::fputs(config_to_toml(cfg_).c_str(), stdout);
            return false;
        }
    }
    if (cfg_.list_models) {
        list_models();
        return false;
    }
    if (cfg_.list_keyboards) {
        std::fputs(keyboard_layout_list().c_str(), stdout);
        return false;
    }
    frontend_entry_ = find_frontend(frontends_, cfg_.frontend);
    if (!frontend_entry_) {
        std::string names;
        for (const FrontendEntry& e : frontends_) names += (names.empty() ? "" : ", ") + e.name;
        return fail(format("unknown front end '%s' (available: %s)", cfg_.frontend.c_str(), names.c_str()));
    }
    uint8_t kbd_id1 = 0, kbd_id2 = 0;
    if (!resolve_keyboard_id(cfg_.terminal, kbd_id1, kbd_id2, error)) return fail(error);

    if (!cfg_.log_spec.empty() && !log_configure(cfg_.log_spec, error)) return fail(error);
    if (!cfg_.log_file.empty() && !log_open_file(cfg_.log_file, error)) return fail(error);

    model_ = find_model(cfg_.model);
    if (!model_) return fail("unknown model '" + cfg_.model + "' (see --list-models)");
    if (cfg_.check_roms) {
        exit_code_ = check_roms(*model_, cfg_);
        return false;
    }
    error = model_->can_build();
    if (!error.empty()) return fail(error);
    if (model_->info().status == ModelStatus::RomIncomplete && !cfg_.allow_bad_rom)
        return fail("model " + model_->info().name + " has no complete ROM dump yet; use --allow-bad-rom to try anyway");
    return true;
}

// ROMs, machine, Twinax host, emulator and monitor.
bool Application::build_machine() {
    const ModelInfo& info = model_->info();
    std::string error;

    std::vector<LoadedRom> roms;
    std::vector<std::string> warnings;
    if (!load_roms(cfg_.rom_dir, info.roms, info.known_bad_roms, cfg_.allow_bad_rom, roms, warnings, error))
        return fail(error);
    for (const std::string& w : warnings) LOG_WARN("rom", "%s", w.c_str());

    auto machine = std::make_unique<Machine>(info);
    machine->set_options(cfg_.terminal);
    try {
        model_->build(*machine, roms);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    machine->bus().set_policy(cfg_.unmapped);
    error = cfg_.twinax.validate(info.max_sessions, info.max_printer_sessions);
    if (!error.empty()) return fail(error);

    // The TN5250 bridge: the virtual workstation controller with one TN5250
    // session per display station (src/host/wsc/tn5250_bridge.hpp).
    const std::string model_name = info.name;
    const DisplaySubtype display = cfg_.terminal.display;
    bridge_ = [model_name, display](const TwinaxConfig& tw, const Tn5250Config& tn, std::string& err) {
        wsc::BridgeOptions bo;
        bo.model = model_name;
        bo.display = display;
        return wsc::make_tn5250_bridge(tw, tn, std::move(bo), err);
    };
    std::unique_ptr<TwinaxHost> host = make_twinax_host(cfg_.twinax, cfg_.tn5250, error, bridge_);
    if (!host) return fail(error);
    if (cfg_.twinax.host == TwinaxHostKind::Tn5250)
        for (const StationConfig& st : cfg_.twinax.effective_stations())
            if (st.type == StationType::Display)
                LOG_INFO("tn5250", "station %u: terminal type %s", st.address,
                         station_terminal_type(cfg_.tn5250, st, info.name, cfg_.terminal.display).c_str());
    machine->set_twinax_host(std::move(host));
    machine->reset();
    uint8_t kbd_id1 = 0, kbd_id2 = 0;
    resolve_keyboard_id(cfg_.terminal, kbd_id1, kbd_id2, error);   // checked in configure()
    LOG_INFO("machine", "%s ready, CPU %s, reset vector at %06X", info.description.c_str(), cpu_level_name(info.cpu),
             machine->cpu().pc_linear());
    LOG_INFO("machine", "display %s, keyboard %s/%s, ID %02X %02X%s", display_subtype_name(cfg_.terminal.display),
             cfg_.terminal.keyboard_type.c_str(), cfg_.terminal.keyboard_layout.c_str(), kbd_id1, kbd_id2,
             cfg_.terminal.keyboard_id >= 0 ? " (override)" : "");

    emu_ = std::make_unique<Emulator>(std::move(machine));
    emu_->set_speed_percent(cfg_.speed_percent);
    monitor_ = std::make_unique<Monitor>(*emu_);
    {
        Monitor::ConnectContext cc;
        cc.base = cfg_;
        cc.max_sessions = info.max_sessions;
        cc.max_printers = int(info.max_printer_sessions);
        cc.factory = bridge_;
        monitor_->set_connect_context(std::move(cc));
    }
    {
        CrtParams cp;
        cp.enabled = cfg_.crt;
        cp.beam_sigma = cfg_.crt_beam;
        monitor_->set_crt_params(cp);
    }
    Monitor* monitor = monitor_.get();
    emu_->set_stop_callback([monitor](const StopEvent& ev) { monitor->notify_stop(ev); });
    emu_->start();
    return true;
}

// GDB stub and the start-up script; then let the machine run.
bool Application::start_debuggers() {
    std::string error;
    if (cfg_.gdb) {
        gdb_ = std::make_unique<GdbStub>(*emu_, monitor_.get());
        if (!gdb_->start(cfg_.gdb_port, error)) return fail("gdb: " + error);
    }

    if (!cfg_.rc_file.empty()) {
        const bool ok = monitor_->run_script(cfg_.rc_file);
        if (!ok && cfg_.batch) {
            exit_code_ = 2;
            return false;
        }
    }
    if (cfg_.batch || monitor_->quit_requested() || emu_->quit_requested()) {
        exit_code_ = emu_->quit_requested() ? emu_->exit_code() : (monitor_->failures() ? 1 : 0);
        return false;
    }

    if (cfg_.gdb_wait) gdb_->wait_for_client();
    else if (!cfg_.start_stopped && cfg_.rc_file.empty()) emu_->resume();
    return true;
}

FrontendOptions Application::frontend_options() const {
    FrontendOptions fo;
    fo.scale = cfg_.scale;
    fo.crt.enabled = cfg_.crt;
    fo.crt.beam_sigma = cfg_.crt_beam;
    if (model_)
        fo.title = "tn34xx: IBM " + model_->info().name + " (" + display_subtype_name(cfg_.terminal.display) + ")";
    return fo;
}

// --- run loop -----------------------------------------------------------------

int Application::run() {
    if (!emu_) return finish();
    return frontend_entry_->create ? run_frontend() : run_headless();
}

int Application::run_headless() {
    if (stdin_is_tty() && console_factory_) {
        console_ = console_factory_(*monitor_, *emu_, cfg_.history_file);
        console_->run();
    } else {
        // Commands from a pipe: execute line by line.
        char buf[4096];
        while (!monitor_->quit_requested() && !emu_->quit_requested() && std::fgets(buf, sizeof buf, stdin))
            monitor_->execute(buf);
    }
    exit_code_ = emu_->quit_requested() ? emu_->exit_code() : (monitor_->failures() ? 1 : 0);
    return finish();
}

int Application::run_frontend() {
    start_console();
    std::unique_ptr<Frontend> frontend = frontend_entry_->create();
    std::string error;
    if (!frontend->run(*emu_, frontend_options(), error)) {
        LOG_ERROR("video", "%s; use --headless", error.c_str());
        emu_->request_quit(2);
    }
    emu_->request_quit(emu_->exit_code());
    stop_console();
    exit_code_ = emu_->quit_requested() ? emu_->exit_code() : (monitor_->failures() ? 1 : 0);
    return finish();
}

void Application::start_console() {
    if (console_thread_.joinable() || !emu_ || !console_factory_ || !stdin_is_tty()) return;
    console_ = console_factory_(*monitor_, *emu_, cfg_.history_file);
    if (!console_) return;
    Console* c = console_.get();
    console_thread_ = std::thread([c] { c->run(); });
}

void Application::stop_console() {
    if (!console_thread_.joinable()) return;
    console_->interrupt();
    console_thread_.join();
}

void Application::request_quit(int exit_code) {
    if (emu_) emu_->request_quit(exit_code);
}

int Application::finish() {
    if (finished_) return exit_code_;
    finished_ = true;
    if (emu_) emu_->request_quit(emu_->quit_requested() ? emu_->exit_code() : exit_code_);
    stop_console();
    if (gdb_) gdb_->stop();
    if (emu_) emu_->shutdown();
    return exit_code_;
}

} // namespace tn34xx
