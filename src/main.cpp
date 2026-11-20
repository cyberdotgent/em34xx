// tn34xx: hardware emulator for IBM 34xx-family 5250 terminals.
#include "config/config.hpp"
#include "frontend/sdl_frontend.hpp"
#include "gdb/gdb_stub.hpp"
#include "machine/emulator.hpp"
#include "models/model.hpp"
#include "monitor/monitor.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
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

using namespace tn34xx;

namespace {

const char* kVersion = "0.0.0";

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

int main(int argc, char** argv) {
    AppConfig cfg;
    std::string error;
    if (!parse_command_line(argc, argv, cfg, error)) {
        std::fprintf(stderr, "tn34xx: %s\n", error.c_str());
        return 2;
    }
    if (cfg.show_help) {
        std::fputs(usage_text(argv[0]).c_str(), stdout);
        return 0;
    }
    if (cfg.show_version) {
        std::printf("tn34xx %s\n", kVersion);
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--dump-config")) {
            std::fputs(config_to_toml(cfg).c_str(), stdout);
            return 0;
        }
    }
    if (cfg.list_models) {
        list_models();
        return 0;
    }

    if (!cfg.log_spec.empty() && !log_configure(cfg.log_spec, error)) {
        std::fprintf(stderr, "tn34xx: %s\n", error.c_str());
        return 2;
    }
    if (!cfg.log_file.empty() && !log_open_file(cfg.log_file, error)) {
        std::fprintf(stderr, "tn34xx: %s\n", error.c_str());
        return 2;
    }

    const Model* model = find_model(cfg.model);
    if (!model) {
        std::fprintf(stderr, "tn34xx: unknown model '%s' (see --list-models)\n", cfg.model.c_str());
        return 2;
    }
    if (cfg.check_roms) return check_roms(*model, cfg);

    error = model->can_build();
    if (!error.empty()) {
        std::fprintf(stderr, "tn34xx: %s\n", error.c_str());
        return 2;
    }
    if (model->info().status == ModelStatus::RomIncomplete && !cfg.allow_bad_rom) {
        std::fprintf(stderr, "tn34xx: model %s has no complete ROM dump yet; use --allow-bad-rom to try anyway\n",
                     model->info().name.c_str());
        return 2;
    }

    // ROMs.
    std::vector<LoadedRom> roms;
    std::vector<std::string> warnings;
    if (!load_roms(cfg.rom_dir, model->info().roms, model->info().known_bad_roms, cfg.allow_bad_rom, roms, warnings,
                   error)) {
        std::fprintf(stderr, "tn34xx: %s\n", error.c_str());
        return 2;
    }
    for (const std::string& w : warnings) LOG_WARN("rom", "%s", w.c_str());

    // Machine.
    auto machine = std::make_unique<Machine>(model->info());
    model->build(*machine, roms);
    machine->bus().set_policy(cfg.unmapped);
    machine->set_options(cfg.terminal);
    std::unique_ptr<TwinaxHost> host = make_twinax_host(cfg.twinax, cfg.tn5250, error);
    if (!host) {
        std::fprintf(stderr, "tn34xx: %s\n", error.c_str());
        return 2;
    }
    machine->set_twinax_host(std::move(host));
    machine->reset();
    LOG_INFO("machine", "%s ready, CPU %s, reset vector at %06X", model->info().description.c_str(),
             cpu_level_name(model->info().cpu), machine->cpu().pc_linear());
    LOG_INFO("machine", "display %s, keyboard %s/%s%s", display_subtype_name(cfg.terminal.display),
             cfg.terminal.keyboard_type.c_str(), cfg.terminal.keyboard_layout.c_str(),
             cfg.terminal.keyboard_id >= 0 ? format(" (ID override %02X)", cfg.terminal.keyboard_id).c_str() : "");

    Emulator emu(std::move(machine));
    emu.set_speed_percent(cfg.speed_percent);
    Monitor monitor(emu);
    emu.set_stop_callback([&monitor](const StopEvent& ev) { monitor.notify_stop(ev); });
    emu.start();

    std::unique_ptr<GdbStub> gdb;
    if (cfg.gdb) {
        gdb = std::make_unique<GdbStub>(emu, &monitor);
        if (!gdb->start(cfg.gdb_port, error)) {
            std::fprintf(stderr, "tn34xx: gdb: %s\n", error.c_str());
            return 2;
        }
    }

    // Start-up script.
    if (!cfg.rc_file.empty()) {
        const bool ok = monitor.run_script(cfg.rc_file);
        if (!ok && cfg.batch) {
            emu.shutdown();
            return 2;
        }
    }
    if (cfg.batch || monitor.quit_requested() || emu.quit_requested()) {
        const int code = emu.quit_requested() ? emu.exit_code() : (monitor.failures() ? 1 : 0);
        if (gdb) gdb->stop();
        emu.shutdown();
        return code;
    }

    if (cfg.gdb_wait) gdb->wait_for_client();
    else if (!cfg.start_stopped && cfg.rc_file.empty()) emu.resume();

    const bool tty = tn_isatty(tn_fileno(stdin)) != 0;
    if (cfg.headless) {
        if (tty) {
            monitor.interactive(cfg.history_file);
        } else {
            // Commands from a pipe: execute line by line.
            char buf[4096];
            while (!monitor.quit_requested() && !emu.quit_requested() && std::fgets(buf, sizeof buf, stdin))
                monitor.execute(buf);
        }
    } else {
        std::thread monitor_thread;
        if (tty) monitor_thread = std::thread([&] { monitor.interactive(cfg.history_file); });
        FrontendOptions fo;
        fo.scale = cfg.scale;
        fo.title = "tn34xx: IBM " + model->info().name + " (" + display_subtype_name(cfg.terminal.display) + ")";
        if (!run_sdl_frontend(emu, fo, error)) {
            LOG_ERROR("video", "%s; use --headless", error.c_str());
            emu.request_quit(2);
        }
        emu.request_quit(emu.exit_code());
        if (monitor_thread.joinable()) {
            monitor.interrupt_input();
            monitor_thread.join();
        }
    }

    const int code = emu.quit_requested() ? emu.exit_code() : (monitor.failures() ? 1 : 0);
    if (gdb) gdb->stop();
    emu.shutdown();
    return code;
}
