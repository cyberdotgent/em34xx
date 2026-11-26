// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/application.hpp"
#include "em34xx_version.h"

#include "app/host_link.hpp"
#include "host/twinax/wsc/tn5250_bridge.hpp"
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
        for (const ModelVariant& v : i.variants) {
            std::string tubes;
            for (const Phosphor& p : v.phosphors) tubes += (tubes.empty() ? "" : ", ") + p.id;
            std::printf("         %-8s %s%s%s%s\n", v.id.c_str(), variant_label(i, v).c_str(), tubes.empty() ? "" : "; phosphor ",
                        tubes.c_str(), &v == &i.variants.front() ? " (default)" : "");
            if (!v.roms.empty()) {
                std::string files;
                for (const RomSpec& r : v.roms) files += (files.empty() ? "" : ", ") + r.file;
                std::printf("                  own ROM images: %s\n", files.c_str());
            }
        }
        for (const ModelOption& o : i.options)
            std::printf("         --set %s=%s  %s (default %s)\n", o.id.c_str(), o.default_on ? "off" : "on",
                        o.description.c_str(), o.default_on ? "on" : "off");
    }
}

int check_roms(const Model& model, const ModelVariant& variant, const AppConfig& cfg) {
    const ModelInfo& info = model.info();
    std::vector<LoadedRom> roms;
    std::vector<std::string> warnings;
    std::string error;
    const bool ok = load_roms(cfg.rom_dir, info.roms_of(variant), info.known_bad_roms, true, roms, warnings, error);
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

    const ModelVariant* named = nullptr;
    if (!find_model_variant(cfg_.model, model_, named))
        return fail("unknown model or variant '" + cfg_.model + "' (see --list-models)");
    error = model_->can_build();
    if (!error.empty()) return fail(error);
    if (model_->info().status == ModelStatus::RomIncomplete && !cfg_.allow_bad_rom)
        return fail("model " + model_->info().name + " has no complete ROM dump yet; use --allow-bad-rom to try anyway");
    if (!select_variant(named, error)) return fail(error);
    if (cfg_.check_roms) {
        exit_code_ = check_roms(*model_, model_->variant_for(cfg_.terminal), cfg_);
        return false;
    }
    // Switches are the model's own: one it does not have does nothing.
    for (const auto& kv : cfg_.terminal.switches) {
        bool known = false;
        for (const ModelOption& o : model_->info().options) known = known || o.id == kv.first;
        if (!known)
            LOG_WARN("machine", "model %s has no option '%s'; ignored (see --list-models)",
                     model_->info().name.c_str(), kv.first.c_str());
    }
    return true;
}

// The variant and phosphor to run: the variant named with the model
// ("3487-HC"), else what --display of older versions means for this model,
// else the model's first; and the phosphor asked for, which the variant must
// offer.
bool Application::select_variant(const ModelVariant* named, std::string& error) {
    const std::string model = model_->info().name;
    if (!named) {
        error = "model " + model + " has no variant to run";
        return false;
    }
    const bool explicit_variant = to_lower(cfg_.model).find(to_lower(named->id)) != std::string::npos;
    const ModelVariant* v = named;
    if (!cfg_.legacy_display.empty()) {
        const bool colour = cfg_.legacy_display == "color";
        if (!explicit_variant) {
            v = model_->variant_with_colour(colour);
            if (!v) {
                error = "model " + model + " has no " + (colour ? "colour" : "monochrome") + " variant (see --list-models)";
                return false;
            }
        }
        if (!colour && cfg_.terminal.phosphor.empty()) cfg_.terminal.phosphor = cfg_.legacy_display;
    }
    if (v->colour) {
        cfg_.terminal.phosphor.clear();
    } else {
        const Phosphor* p = v->phosphor(cfg_.terminal.phosphor);
        if (!cfg_.terminal.phosphor.empty() && to_lower(p->id) != to_lower(cfg_.terminal.phosphor)) {
            std::string list;
            for (const Phosphor& o : v->phosphors) list += (list.empty() ? "" : ", ") + o.id;
            error = v->id + " has no phosphor '" + cfg_.terminal.phosphor + "' (" + list + ")";
            return false;
        }
        cfg_.terminal.phosphor = p->id;
    }
    cfg_.terminal.variant = v->id;
    cfg_.model = model;
    return true;
}

namespace {
// What the tube of a unit built with these options shows.
Machine::Tube tube_for(const Model& model, const TerminalOptions& options) {
    const ModelVariant& v = model.variant_for(options);
    Machine::Tube t;
    t.colour = v.colour;
    if (const Phosphor* p = v.phosphor(options.phosphor)) t.phosphor = p->rgb;
    return t;
}

std::string tube_name(const Model& model, const TerminalOptions& options) {
    const ModelVariant& v = model.variant_for(options);
    const Phosphor* p = v.phosphor(options.phosphor);
    return p ? p->id : "colour";
}
} // namespace

// ROMs and a machine built with these terminal options, reset; no Twinax
// host yet.
std::unique_ptr<Machine> Application::make_machine(const Model& model, const TerminalOptions& options,
                                                   std::string& error) {
    const ModelInfo& info = model.info();
    std::vector<LoadedRom> roms;
    std::vector<std::string> warnings;
    if (!load_roms(cfg_.rom_dir, info.roms_of(model.variant_for(options)), info.known_bad_roms, cfg_.allow_bad_rom, roms, warnings, error))
        return nullptr;
    for (const std::string& w : warnings) LOG_WARN("rom", "%s", w.c_str());

    auto machine = std::make_unique<Machine>(info);
    machine->set_options(options);
    machine->set_tube(tube_for(model, options));
    try {
        model.build(*machine, roms);
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

    std::unique_ptr<Machine> machine = make_machine(*model_, cfg_.terminal, error);
    if (!machine) return fail(error);
    error = cfg_.twinax.validate(info.max_sessions, info.max_printer_sessions);
    if (!error.empty()) return fail(error);

    // The TN5250 bridge: the virtual workstation controller with one TN5250
    // session per display station (src/host/twinax/wsc/tn5250_bridge.hpp).
    bridge_variant_ = std::make_shared<std::atomic<const ModelVariant*>>(&model_->variant_for(cfg_.terminal));
    auto bridge_variant = bridge_variant_;
    // The front end's TLS trust handler (HostLink::set_tls_trust_handler)
    // reaches each new bridge through the broker.
    auto trust = std::make_shared<TlsTrustBroker>();
    Tn5250HostFactory bridge = [bridge_variant, trust](const TwinaxConfig& tw, const Tn5250Config& tn, std::string& err) {
        wsc::BridgeOptions bo;
        bo.terminal_type = bridge_variant->load()->tn5250_type;
        bo.tls_trust = trust->for_new_bridge();
        return wsc::make_tn5250_bridge(tw, tn, std::move(bo), err);
    };
    // The controller for the model's line. Only Twinax has one.
    std::unique_ptr<LineHost> host;
    if (info.line == LineKind::Twinax) {
        host = make_twinax_host(cfg_.twinax, cfg_.tn5250, error, bridge);
        if (!host) return fail(error);
    } else if (cfg_.twinax.host != TwinaxHostKind::None) {
        return fail(std::string("there is no controller for ") + line_kind_name(info.line) + " terminals in this build");
    } else {
        host = make_null_line_host(info.line);
    }
    if (cfg_.twinax.host == TwinaxHostKind::Tn5250)
        for (const StationConfig& st : cfg_.twinax.effective_stations())
            if (st.type == StationType::Display)
                LOG_INFO("tn5250", "station %u: terminal type %s", st.address,
                         station_terminal_type(cfg_.tn5250, st, model_->variant_for(cfg_.terminal).tn5250_type).c_str());
    machine->set_line_host(std::move(host));
    machine->reset();
    uint8_t kbd_id1 = 0, kbd_id2 = 0;
    resolve_keyboard_id(cfg_.terminal, kbd_id1, kbd_id2, error);   // checked in configure()
    LOG_INFO("machine", "%s ready, CPU %s, reset vector at %06X", info.description.c_str(), cpu_level_name(info.cpu),
             machine->cpu().pc_linear());
    LOG_INFO("machine", "variant %s, display %s, keyboard %s/%s, ID %02X %02X%s", cfg_.terminal.variant.c_str(),
             tube_name(*model_, cfg_.terminal).c_str(),
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
        fo.title = "em34xx: IBM " + model_->variant_for(cfg_.terminal).id + " (" + tube_name(*model_, cfg_.terminal) + ")";
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

// The host connection follows the terminal's life. A reset or power-off
// ends the sessions the terminal was using (Machine::reset() tells the
// controller; power-off does here), and a reset or power-up dials the host
// it was last connected to again when the user had disconnected.
void Application::redial() {
    std::string message, error;
    if (host_link_ && !host_link_->reconnect(message, error) && !error.empty())
        LOG_WARN("machine", "%s", error.c_str());
}

void Application::power_on() {
    emu_->request_stop();
    emu_->wait_stopped();
    emu_->with_machine([](Machine& m) { m.reset(); });
    redial();
    emu_->resume();
}

void Application::power_off() {
    emu_->request_stop();
    emu_->wait_stopped();
    // Switched off: its sessions are over. No time passes for a stopped
    // machine, so new ones only start at the next power-up.
    emu_->with_machine([](Machine& m) {
        if (m.line_host()) m.line_host()->terminal_reset();
    });
}

void Application::reset() {
    const bool was_running = emu_->running();
    if (was_running) {
        emu_->request_stop();
        emu_->wait_stopped();
    }
    emu_->with_machine([](Machine& m) { m.reset(); });
    redial();
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
    emu_->request_stop();
    emu_->wait_stopped();
    emu_->with_machine([](Machine& m) { m.reset(); });
    LOG_INFO("machine", "power cycle before connecting: the terminal runs its self test");
    const bool ok = host_link_->connect(cfg, message, error);
    emu_->resume();
    return ok;
}

const ModelVariant& Application::variant() const { return model_->variant_for(cfg_.terminal); }

bool Application::set_phosphor(const std::string& id, std::string& error) {
    if (!emu_) {
        error = "no machine";
        return false;
    }
    const ModelVariant& v = variant();
    const Phosphor* p = v.phosphor(id);
    if (!p || to_lower(p->id) != to_lower(id)) {
        error = v.id + " has no phosphor '" + id + "'";
        return false;
    }
    if (cfg_.terminal.phosphor == p->id) return true;
    // Only the picture: the presentation (render_presented) reads the tube
    // from the machine.
    cfg_.terminal.phosphor = p->id;
    const Machine::Tube tube = tube_for(*model_, cfg_.terminal);
    const std::string phosphor = p->id;
    emu_->with_machine([&](Machine& m) {
        TerminalOptions o = m.options();
        o.phosphor = phosphor;
        m.set_options(o);
        m.set_tube(tube);
    });
    LOG_INFO("machine", "phosphor %s (picture only, no reset)", p->id.c_str());
    return true;
}

bool Application::rebuild_machine(const TerminalOptions& options, std::string& error) {
    return rebuild(*model_, options, error);
}

bool Application::set_model(const Model& model, const TerminalOptions& options, std::string& error) {
    error = model.can_build();
    if (error.empty() && model.info().status != ModelStatus::Bringup)
        error = "model " + model.info().name + " cannot run yet (" + model_status_name(model.info().status) + ")";
    if (!error.empty()) return false;
    return rebuild(model, options, error);
}

bool Application::rebuild(const Model& model, const TerminalOptions& options, std::string& error) {
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
    std::unique_ptr<Machine> machine = make_machine(model, options, error);
    if (!machine) {
        LOG_ERROR("machine", "rebuild failed: %s", error.c_str());
        emu_->resume();
        return false;
    }
    // Any other controller (a script host, nothing) moves to the new
    // machine; the TN5250 bridge was taken off above.
    // (Between machines on the same kind of line; one for another line
    // stays behind and is destroyed with the old machine.)
    std::unique_ptr<LineHost> host = emu_->with_machine([](Machine& m) {
        return m.swap_line_host(make_null_line_host(m.line()));
    });
    if (host) machine->set_line_host(std::move(host));
    machine->reset();
    std::unique_ptr<Machine> old = emu_->replace_machine(std::move(machine));
    old.reset();
    cfg_.terminal = options;
    // The options name the variant and phosphor that were built.
    const ModelVariant& variant = model.variant_for(options);
    cfg_.terminal.variant = variant.id;
    const Phosphor* phosphor = variant.phosphor(options.phosphor);
    cfg_.terminal.phosphor = phosphor ? phosphor->id : std::string();
    if (bridge_variant_) bridge_variant_->store(&variant);
    if (&model != model_) {
        // Another terminal: new sessions announce its type, and its limits
        // apply to the next connection.
        model_ = &model;
        cfg_.model = model.info().name;
        host_link_->set_model(model.info().name, model.info().max_sessions, int(model.info().max_printer_sessions));
    }
    LOG_INFO("machine", "rebuilt as IBM %s with display %s; power on", variant.id.c_str(),
             tube_name(model, cfg_.terminal).c_str());
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
