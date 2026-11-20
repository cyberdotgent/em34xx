#include "frontend/sdl_frontend.hpp"

#include "machine/emulator.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <SDL3/SDL.h>

namespace tn34xx {

namespace {

struct SdlSession {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int tex_w = 0, tex_h = 0;

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
    emu.with_machine([&](Machine& m) { m.render(fb); });

    s.window = SDL_CreateWindow(opts.title.c_str(), fb.width * opts.scale, fb.height * opts.scale, SDL_WINDOW_RESIZABLE);
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

        emu.with_machine([&](Machine& m) { m.render(fb); });
        if (!s.texture || s.tex_w != fb.width || s.tex_h != fb.height) {
            if (s.texture) SDL_DestroyTexture(s.texture);
            s.texture = SDL_CreateTexture(s.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                          fb.width, fb.height);
            if (!s.texture) {
                error = std::string("SDL_CreateTexture failed: ") + SDL_GetError();
                return false;
            }
            SDL_SetTextureScaleMode(s.texture, SDL_SCALEMODE_NEAREST);
            SDL_SetRenderLogicalPresentation(s.renderer, fb.width, fb.height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
            s.tex_w = fb.width;
            s.tex_h = fb.height;
        }
        SDL_UpdateTexture(s.texture, nullptr, fb.pixels.data(), fb.width * 4);
        SDL_SetRenderDrawColor(s.renderer, 0, 0, 0, 255);
        SDL_RenderClear(s.renderer);
        SDL_RenderTexture(s.renderer, s.texture, nullptr, nullptr);
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
