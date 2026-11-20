#include "frontend/sdl_frontend.hpp"

#include "machine/emulator.hpp"
#include "present/display_profile.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>

namespace tn34xx {

namespace {

struct SdlSession {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int tex_w = 0, tex_h = 0;
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

} // namespace

bool run_sdl_frontend(Emulator& emu, const FrontendOptions& opts, std::string& error) {
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

    std::string last_title;
    while (!emu.quit_requested()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                emu.request_quit(emu.exit_code());
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                if (e.key.repeat) break;   // the terminal does its own typematic
                HostKeyEvent ev;
                ev.hid_usage = uint32_t(e.key.scancode);
                ev.pressed = e.type == SDL_EVENT_KEY_DOWN;
                ev.name = SDL_GetScancodeName(e.key.scancode);
                emu.with_machine([&](Machine& m) { m.key_event(ev); });
                break;
            }
            default:
                break;
            }
        }

        emu.with_machine([&](Machine& m) { render_presented(m, fb); });

        // Draw the tube at the window's real pixel size so the scan-line
        // structure is computed at the output resolution.
        int win_px_w = 0, win_px_h = 0;
        SDL_GetWindowSizeInPixels(s.window, &win_px_w, &win_px_h);
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
            crt_render(fb, out_w, out_h, opts.crt, s.image);
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

        const std::string title = format("%s - %s", opts.title.c_str(), emu.running() ? "running" : "stopped");
        if (title != last_title) {
            SDL_SetWindowTitle(s.window, title.c_str());
            last_title = title;
        }
        SDL_Delay(16);
    }
    return true;
}

} // namespace tn34xx
