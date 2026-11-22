// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/application.hpp"
#include "em34xx_version.h"

#include "app/host_link.hpp"
#include "host/wsc/tn5250_bridge.hpp"
#include "machine/emulator.hpp"
#include "models/model.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <cstdio>
#include <cstring>
#include <exception>

namespace em34xx {

namespace {

const char* kVersion = EM34XX_VERSION_STRING;

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

bool AppTooling::requested(const AppConfig& cfg) {
    return !cfg.rc_file.empty() || cfg.batch || cfg.start_stopped || cfg.gdb || cfg.gdb_wait || cfg.headless ||
           !cfg.history_file.empty();
}

Application::Application(std::unique_ptr<Frontend> frontend, std::unique_ptr<AppTooling> tooling)
    : frontend_(std::move(frontend)), tooling_(std::move(tooling)) {}

Application::~Application() { finish(); }

int Application::main(int argc, char** argv) {
    if (!start(argc, argv)) {
        finish();
        return exit_code_;
    }
    return run();
}

bool Application::fail(const std::string& message, int code) {
    std::fprintf(stderr, "em34xx: %s\n", message.c_str());
    last_error_ = message;
    exit_code_ = code;
    return false;
}

// --- start-up ---------------------------------------------------------------

bool Application::start(int argc, char** argv) {
    exit_code_ = 0;
    if (!configure(argc, argv)) return false;
    if (!build_machine()) return false;
    if (tooling_) return tooling_->start(*this, exit_code_);
    emu_->resume();
    return true;
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
        std::printf("em34xx %s\n", kVersion);
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
    if (!tooling_ && AppTooling::requested(cfg_))
        return fail("the monitor, scripts and the GDB stub are not part of this build; use em34xx or em34xx-headless");
    if (!frontend_ && !tooling_) return fail("this build has neither a window nor a monitor");
    if (!frontend_) cfg_.headless = true;
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

// ROMs and a machine built with these terminal options, reset; no Twinax
// host yet.
std::unique_ptr<Machine> Application::make_machine(const TerminalOptions& options, std::string& error) {
    const ModelInfo& info = model_->info();
    std::vector<LoadedRom> roms;
    std::vector<std::string> warnings;
    if (!load_roms(cfg_.rom_dir, info.roms, info.known_bad_roms, cfg_.allow_bad_rom, roms, warnings, error))
        return nullptr;
    for (const std::string& w : warnings) LOG_WARN("rom", "%s", w.c_str());

    auto machine = std::make_unique<Machine>(info);
    machine->set_options(options);
    try {
        model_->build(*machine, roms);
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
    machine->bus().set_policy(cfg_.unmapped);
    return machine;
}

// ROMs, machine, Twinax host and emulator.
bool Application::build_machine() {
    const ModelInfo& info = model_->info();
    std::string error;

    std::unique_ptr<Machine> machine = make_machine(cfg_.terminal, error);
    if (!machine) return fail(error);
    error = cfg_.twinax.validate(info.max_sessions, info.max_printer_sessions);
    if (!error.empty()) return fail(error);

    // The TN5250 bridge: the virtual workstation controller with one TN5250
    // session per display station (src/host/wsc/tn5250_bridge.hpp).
    const std::string model_name = info.name;
    bridge_display_ = std::make_shared<std::atomic<DisplaySubtype>>(cfg_.terminal.display);
    auto display = bridge_display_;
    // The front end's TLS trust handler (HostLink::set_tls_trust_handler)
    // reaches each new bridge through the broker.
    auto trust = std::make_shared<TlsTrustBroker>();
    Tn5250HostFactory bridge = [model_name, display, trust](const TwinaxConfig& tw, const Tn5250Config& tn,
                                                            std::string& err) {
        wsc::BridgeOptions bo;
        bo.model = model_name;
        bo.display = display->load();
        bo.tls_trust = trust->for_new_bridge();
        return wsc::make_tn5250_bridge(tw, tn, std::move(bo), err);
    };
    std::unique_ptr<TwinaxHost> host = make_twinax_host(cfg_.twinax, cfg_.tn5250, error, bridge);
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
    host_link_ = std::make_unique<HostLink>(*emu_, cfg_, info.max_sessions, int(info.max_printer_sessions),
                                            std::move(bridge), trust);
    AppTooling* tooling = tooling_.get();
    if (tooling) emu_->set_stop_callback([tooling](const StopEvent& ev) { tooling->on_stop(ev); });
    emu_->start();
    return true;
}

FrontendOptions Application::frontend_options() const {
    FrontendOptions fo;
    fo.scale = cfg_.scale;
    fo.crt.enabled = cfg_.crt;
    fo.crt.beam_sigma = cfg_.crt_beam;
    if (model_)
        fo.title = "em34xx: IBM " + model_->info().name + " (" + display_subtype_name(cfg_.terminal.display) + ")";
    return fo;
}

// --- run loop -----------------------------------------------------------------

int Application::run() {
    if (!emu_) return finish();
    if (cfg_.headless || !frontend_) {
        tooling_->run_headless(*this);
    } else {
        if (tooling_) {
            tooling_->start_background(*this);
            background_ = true;
        }
        std::string error;
        if (!frontend_->run(*this, frontend_options(), error)) {
            LOG_ERROR("video", "%s; use --headless", error.c_str());
            emu_->request_quit(2);
        }
        emu_->request_quit(emu_->exit_code());
    }
    exit_code_ = final_status();
    return finish();
}

int Application::final_status() const {
    return emu_->quit_requested() ? emu_->exit_code() : (tooling_ ? tooling_->exit_status() : 0);
}

void Application::request_quit(int exit_code) {
    if (emu_) emu_->request_quit(exit_code);
    if (frontend_) frontend_->request_quit();
}

int Application::finish() {
    if (finished_) return exit_code_;
    finished_ = true;
    if (emu_) emu_->request_quit(emu_->quit_requested() ? emu_->exit_code() : exit_code_);
    if (tooling_) {
        if (background_) tooling_->stop_background();
        tooling_->shutdown();
    }
    if (emu_) emu_->shutdown();
    return exit_code_;
}

// --- control API ----------------------------------------------------------------

void Application::power_on() {
    emu_->request_stop();
    emu_->wait_stopped();
    emu_->with_machine([](Machine& m) { m.reset(); });
    emu_->resume();
}

void Application::power_off() {
    emu_->request_stop();
    emu_->wait_stopped();
}

void Application::reset() {
    const bool was_running = emu_->running();
    if (was_running) {
        emu_->request_stop();
        emu_->wait_stopped();
    }
    emu_->with_machine([](Machine& m) { m.reset(); });
    if (was_running) emu_->resume();
}

bool Application::powered() const { return emu_ && emu_->running(); }

bool Application::connect_after_power_cycle(const AppConfig& cfg, std::string& message, std::string& error) {
    if (!emu_) {
        error = "connect: no machine";
        return false;
    }
    // Close the running sessions first, so no session sees the terminal
    // vanish mid-screen ("not connected" is fine).
    std::string ignored;
    host_link_->disconnect(ignored);
    power_on();
    LOG_INFO("machine", "power cycle before connecting: the terminal runs its self test");
    return host_link_->connect(cfg, message, error);
}

bool Application::display_change_needs_power_cycle(DisplaySubtype to) const {
    if (!emu_ || !model_) return false;
    const TerminalOptions current = emu_->with_machine([](Machine& m) { return m.options(); });
    return model_->display_change_needs_power_cycle(current, to);
}

bool Application::set_display_subtype(DisplaySubtype to, std::string& error) {
    if (!emu_) {
        error = "no machine";
        return false;
    }
    TerminalOptions options = emu_->with_machine([](Machine& m) { return m.options(); });
    if (options.display == to) return true;
    if (model_->display_change_needs_power_cycle(options, to)) {
        options.display = to;
        return rebuild_machine(options, error);
    }
    // Only the picture: the presentation (render_presented) reads the
    // subtype from the machine's options; mark the picture changed.
    emu_->with_machine([to](Machine& m) {
        TerminalOptions o = m.options();
        o.display = to;
        m.set_options(o);
        m.mark_display_changed();
    });
    cfg_.terminal.display = to;
    if (bridge_display_) bridge_display_->store(to);
    LOG_INFO("machine", "display %s (picture only, no reset)", display_subtype_name(to));
    return true;
}

bool Application::rebuild_machine(const TerminalOptions& options, std::string& error) {
    if (!emu_) {
        error = "no machine";
        return false;
    }
    std::string ignored;
    host_link_->disconnect(ignored);   // the sessions close (their terminal type may change)
    emu_->request_stop();
    emu_->wait_stopped();
    // The new machine reads the battery-backed RAM image at build time:
    // write the old machine's first (it is written again when the old one
    // is destroyed, with the same contents, since it is stopped).
    emu_->with_machine([](Machine& m) {
        std::string err;
        if (!m.flush_nonvolatile(err)) LOG_ERROR("nvram", "%s", err.c_str());
    });
    std::unique_ptr<Machine> machine = make_machine(options, error);
    if (!machine) {
        LOG_ERROR("machine", "rebuild failed: %s", error.c_str());
        emu_->resume();
        return false;
    }
    // Any other controller (a script host, nothing) moves to the new
    // machine; the TN5250 bridge was taken off above.
    std::unique_ptr<TwinaxHost> host =
        emu_->with_machine([](Machine& m) { return m.swap_twinax_host(std::make_unique<NullTwinaxHost>()); });
    machine->set_twinax_host(host ? std::move(host) : std::make_unique<NullTwinaxHost>());
    machine->reset();
    std::unique_ptr<Machine> old = emu_->replace_machine(std::move(machine));
    old.reset();
    cfg_.terminal = options;
    if (bridge_display_) bridge_display_->store(options.display);
    LOG_INFO("machine", "rebuilt with display %s; power on", display_subtype_name(options.display));
    emu_->resume();
    return true;
}

bool Application::flush_persistent_state(std::string& error) {
    if (!emu_) return true;
    const bool ok = emu_->with_machine([&](Machine& m) { return m.flush_nonvolatile(error); });
    if (!ok) LOG_ERROR("nvram", "%s", error.c_str());
    return ok;
}

void Application::set_speed_percent(unsigned pct) {
    cfg_.speed_percent = pct;
    emu_->set_speed_percent(pct);
}

void Application::set_crt(const CrtParams& crt) {
    cfg_.crt = crt.enabled;
    cfg_.crt_beam = crt.beam_sigma;
}

} // namespace em34xx
