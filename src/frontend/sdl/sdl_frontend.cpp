#include "frontend/sdl/sdl_frontend.hpp"

#include "app/application.hpp"
#include "machine/emulator.hpp"
#include "machine/model_info.hpp"
#include "present/display_profile.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace tn34xx {

namespace {

struct SdlSession {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int tex_w = 0, tex_h = 0;
    int win_px_w = -1, win_px_h = -1;
    // Cache: the CRT image is recomputed only when the raster or the window
    // size changes.
    std::vector<uint32_t> last_raster;
    int last_raster_w = 0, last_raster_h = 0;
    std::vector<uint32_t> image;

    ~SdlSession() {
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

class SdlFrontend : public Frontend {
public:
    const char* name() const override { return "sdl"; }
    bool run(Application& app, const FrontendOptions& opts, std::string& error) override;
    void request_quit() override { quit_.store(true); }
    void set_title(const std::string& title) override {
        std::lock_guard<std::mutex> lock(text_mutex_);
        title_ = title;
        have_title_ = true;
    }
    void set_status(const std::string& status) override {
        std::lock_guard<std::mutex> lock(text_mutex_);
        status_ = status;
    }

private:
    std::string window_title(const Emulator& emu, const FrontendOptions& opts);

    std::atomic<bool> quit_{false};
    std::atomic<bool> wake_pending_{false};
    std::mutex text_mutex_;
    std::string title_;
    bool have_title_ = false;
    std::string status_;
};

std::string SdlFrontend::window_title(const Emulator& emu, const FrontendOptions& opts) {
    std::lock_guard<std::mutex> lock(text_mutex_);
    const std::string& base = have_title_ ? title_ : opts.title;
    std::string t = format("%s - %s", base.c_str(), emu.running() ? "running" : "stopped");
    if (!status_.empty()) t += " - " + status_;
    return t;
}

} // namespace

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// `--log input=debug`: every SDL keyboard event as SDL delivered it, before
// the keymap (whose decisions Set3Keyboard / HostKeymapEngine log next).
void log_sdl_key(const SDL_KeyboardEvent& k) {
    TN_LOG("input", Debug, "sdl %s scancode %u (%s) key %08X (%s) mod %04X%s raw %u window %u t=%.6f s",
           k.down ? "down" : "up  ", unsigned(k.scancode), SDL_GetScancodeName(k.scancode), unsigned(k.key),
           SDL_GetKeyName(k.key), unsigned(k.mod), k.repeat ? " REPEAT" : "", unsigned(k.raw),
           unsigned(k.windowID), double(k.timestamp) / 1e9);
}

// `--log perf=debug`: what the window costs, every couple of seconds.
struct PerfStats {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    uint64_t cycles_start = 0;
    uint64_t wakes = 0, rasters = 0, crt_renders = 0, presents = 0;
    double raster_ms = 0, crt_ms = 0, crt_max_ms = 0;

    void report(Emulator& emu) {
        const double wall_ms = ms_since(start);
        if (wall_ms < 2000.0) return;
        uint64_t cycles = 0, hz = 0;
        emu.with_machine([&](Machine& m) {
            cycles = m.cpu().cycles();
            hz = m.model().clock_hz;
        });
        if (cycles_start != 0 && log_enabled("perf", LogLevel::Debug)) {
            const double mhz = double(cycles - cycles_start) / wall_ms / 1000.0;
            TN_LOG("perf", Debug,
                   "%.1f s: emulated %.2f MHz (%.0f%% of nominal); %llu display changes, %llu rasters "
                   "(%.2f ms avg), %llu CRT renders (%.2f ms avg, %.2f ms max), %llu presents",
                   wall_ms / 1000.0, mhz, hz ? 100.0 * mhz * 1e6 / double(hz) : 0.0, (unsigned long long)wakes,
                   (unsigned long long)rasters, rasters ? raster_ms / double(rasters) : 0.0,
                   (unsigned long long)crt_renders, crt_renders ? crt_ms / double(crt_renders) : 0.0, crt_max_ms,
                   (unsigned long long)presents);
        }
        *this = PerfStats();
        cycles_start = cycles;
    }
};

} // namespace

std::unique_ptr<Frontend> make_sdl_frontend() { return std::make_unique<SdlFrontend>(); }

bool SdlFrontend::run(Application& app, const FrontendOptions& opts, std::string& error) {
    Emulator& emu = app.emulator();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        error = std::string("SDL_Init failed: ") + SDL_GetError();
        return false;
    }
    SdlSession s;

    Framebuffer fb;
    emu.with_machine([&](Machine& m) { render_presented(m, fb); });

    // Initial window: the aspect-corrected frame times the scale factor,
    // shrunk if needed to fit the usable desktop area.
    int win_w = fb.presented_width() * opts.scale;
    int win_h = fb.presented_height() * opts.scale;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0) {
        const double fit = std::min(0.9 * usable.w / win_w, 0.9 * usable.h / win_h);
        if (fit < 1.0) {
            win_w = int(win_w * fit);
            win_h = int(win_h * fit);
        }
    }
    s.window = SDL_CreateWindow(opts.title.c_str(), win_w, win_h, SDL_WINDOW_RESIZABLE);
    if (!s.window) {
        error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        return false;
    }
    s.renderer = SDL_CreateRenderer(s.window, nullptr);
    if (!s.renderer) {
        error = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
        return false;
    }
    SDL_SetRenderVSync(s.renderer, 1);
    LOG_INFO("video", "SDL %d.%d.%d, video driver %s, renderer %s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
             SDL_MICRO_VERSION, SDL_GetCurrentVideoDriver(), SDL_GetRendererName(s.renderer));

    // Wake the event loop as soon as the emulator reports a new picture
    // (Emulator::set_display_callback runs on the emulation thread; pushing
    // an SDL event is thread-safe). One wake-up event in flight at a time.
    const Uint32 wake_event = SDL_RegisterEvents(1);
    if (wake_event != 0) {
        emu.set_display_callback([this, wake_event](uint64_t) {
            if (wake_pending_.exchange(true)) return;
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = wake_event;
            SDL_PushEvent(&ev);
        });
    }
    struct ClearCallback {
        Emulator& emu;
        ~ClearCallback() { emu.set_display_callback(nullptr); }
    } clear_callback{emu};

    std::string last_title;
    uint64_t seen_generation = 0;
    bool dirty = true;   // first frame
    PerfStats perf;
    auto handle = [&](const SDL_Event& e) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            emu.request_quit(emu.exit_code());
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            TN_LOG("input", Debug, "sdl window focus %s t=%.6f s",
                   e.type == SDL_EVENT_WINDOW_FOCUS_GAINED ? "gained" : "lost", double(e.window.timestamp) / 1e9);
            break;
        case SDL_EVENT_KEYMAP_CHANGED:
            TN_LOG("input", Debug, "sdl keymap changed");
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            log_sdl_key(e.key);
            if (e.key.repeat) break;   // the terminal does its own typematic
            HostKeyEvent ev;
            ev.hid_usage = uint32_t(e.key.scancode);
            ev.pressed = e.type == SDL_EVENT_KEY_DOWN;
            ev.name = SDL_GetScancodeName(e.key.scancode);
            emu.with_machine([&](Machine& m) { m.key_event(ev); });
            break;
        }
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_RENDER_TARGETS_RESET:
        case SDL_EVENT_RENDER_DEVICE_RESET:
            dirty = true;
            break;
        default:
            if (wake_event != 0 && e.type == wake_event) wake_pending_.store(false);
            break;
        }
    };
    while (!emu.quit_requested() && !quit_.load()) {
        // Sleep until an event, a new picture, or 16 ms (title updates and
        // a fallback when the wake-up event could not be registered).
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 16)) {
            handle(e);
            while (SDL_PollEvent(&e)) handle(e);
        }

        const std::string title = window_title(emu, opts);
        if (title != last_title) {
            SDL_SetWindowTitle(s.window, title.c_str());
            last_title = title;
        }

        const uint64_t generation = emu.display_generation();
        if (generation != seen_generation) {
            seen_generation = generation;
            dirty = true;
            ++perf.wakes;
        }
        perf.report(emu);
        int win_px_w = 0, win_px_h = 0;
        SDL_GetWindowSizeInPixels(s.window, &win_px_w, &win_px_h);
        if (win_px_w != s.win_px_w || win_px_h != s.win_px_h) {
            s.win_px_w = win_px_w;
            s.win_px_h = win_px_h;
            dirty = true;
        }
        if (!dirty) continue;
        dirty = false;

        auto t0 = std::chrono::steady_clock::now();
        emu.with_machine([&](Machine& m) { render_presented(m, fb); });
        perf.raster_ms += ms_since(t0);
        ++perf.rasters;

        // Draw the tube at the window's real pixel size so the scan-line
        // structure is computed at the output resolution.
        int out_w = 1, out_h = 1;
        crt_fit(fb, std::max(1, win_px_w), std::max(1, win_px_h), out_w, out_h);
        const bool size_changed = !s.texture || s.tex_w != out_w || s.tex_h != out_h;
        const bool content_changed = fb.width != s.last_raster_w || fb.height != s.last_raster_h ||
                                     fb.pixels != s.last_raster;
        if (size_changed) {
            if (s.texture) SDL_DestroyTexture(s.texture);
            s.texture = SDL_CreateTexture(s.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                          out_w, out_h);
            if (!s.texture) {
                error = std::string("SDL_CreateTexture failed: ") + SDL_GetError();
                return false;
            }
            SDL_SetTextureScaleMode(s.texture, SDL_SCALEMODE_NEAREST);
            s.tex_w = out_w;
            s.tex_h = out_h;
        }
        if (size_changed || content_changed) {
            t0 = std::chrono::steady_clock::now();
            crt_render(fb, out_w, out_h, opts.crt, s.image);
            const double crt_ms = ms_since(t0);
            perf.crt_ms += crt_ms;
            perf.crt_max_ms = std::max(perf.crt_max_ms, crt_ms);
            ++perf.crt_renders;
            SDL_UpdateTexture(s.texture, nullptr, s.image.data(), out_w * 4);
            s.last_raster = fb.pixels;
            s.last_raster_w = fb.width;
            s.last_raster_h = fb.height;
        }
        SDL_SetRenderDrawColor(s.renderer, 0, 0, 0, 255);
        SDL_RenderClear(s.renderer);
        // 1:1 in window pixels, centred (letterboxed).
        float scale_x = 1.0f, scale_y = 1.0f;
        SDL_GetRenderScale(s.renderer, &scale_x, &scale_y);
        const SDL_FRect dst{float(win_px_w - out_w) / 2.0f / scale_x, float(win_px_h - out_h) / 2.0f / scale_y,
                            float(out_w) / scale_x, float(out_h) / scale_y};
        SDL_RenderTexture(s.renderer, s.texture, nullptr, &dst);
        SDL_RenderPresent(s.renderer);
        ++perf.presents;
    }
    return true;
}

} // namespace tn34xx
