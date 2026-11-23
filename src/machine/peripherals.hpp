// SPDX-License-Identifier: GPL-3.0-or-later
// Interfaces between a machine model and the host front end.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace em34xx {

// 32-bit pixels, 0xAARRGGBB.
struct Framebuffer {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
    // Physical width/height ratio of the visible raster on the real tube.
    // Terminal pixels are generally not square (e.g. 1280x525 shown on a 4:3
    // CRT), so presentation stretches the frame to this ratio. 0 = square.
    double display_aspect = 0.0;
    // Black border between the edge of the visible tube and the active
    // raster, as a fraction of the tube's width (left and right) and height
    // (top and bottom). Real tubes have unequal horizontal and vertical
    // margins. 0 = the raster fills the presentation.
    double border_x = 0.0;
    double border_y = 0.0;

    // Size of the whole presented tube area (raster plus border) in output
    // pixels, keeping the raster at its native width.
    int presented_width() const {
        return border_x > 0.0 ? int(double(width) / (1.0 - 2.0 * border_x) + 0.5) : width;
    }
    int presented_height() const {
        const int w = presented_width();
        if (display_aspect > 0.0) return int(double(w) / display_aspect + 0.5);
        return border_y > 0.0 ? int(double(height) / (1.0 - 2.0 * border_y) + 0.5) : height;
    }

    void resize(int w, int h) {
        width = w;
        height = h;
        pixels.assign(size_t(w) * size_t(h), 0xFF000000u);
    }
    uint32_t* row(int y) { return pixels.data() + size_t(y) * size_t(width); }
};

// Implemented by a model's display hardware.
class DisplaySource {
public:
    virtual ~DisplaySource() = default;
    // Render the current screen into fb (resizing it if needed).
    virtual void render(Framebuffer& fb) = 0;
    // A value that changes whenever the picture changes over time without
    // a memory or register write (character or cursor blink phase). Writes
    // are tracked separately (Bus::observe_writes), so a device that never
    // animates keeps the default.
    virtual uint64_t animation_phase() const { return 0; }
};

// A key on the host keyboard, identified by its USB HID usage code (which is
// what SDL scancodes are). Models translate this to their own keyboard.
struct HostKeyEvent {
    uint32_t hid_usage = 0;
    bool pressed = false;
    std::string name;   // for logging only
};

// A pointing device the user moves: relative movement in device counts (dy
// positive = towards the user) and the buttons held (bit 0 left, bit 1
// right, bit 2 middle).
class PointerSink {
public:
    virtual ~PointerSink() = default;
    virtual void pointer_event(int dx, int dy, unsigned buttons) = 0;
};

class KeyboardSink {
public:
    virtual ~KeyboardSink() = default;
    virtual void host_key(const HostKeyEvent& ev) = 0;
    // Press or release the terminal's own key for a function ("attn",
    // "sysrq", "reset"... as host keymap files name them), with the
    // terminal Shift/Alt its legend needs, bypassing the host keymap: what
    // a toolbar button does. False if this keyboard has no such key.
    virtual bool terminal_key(const std::string& function, bool pressed) {
        (void)function;
        (void)pressed;
        return false;
    }
    // Replace the host keymap ("pcomm", "positional" or a TOML file, as
    // TerminalOptions::keymap). False with `error` if it does not load or
    // the keyboard has no keymap.
    virtual bool set_host_keymap(const std::string& spec, bool shift_fkeys, std::string& error) {
        (void)spec;
        (void)shift_fkeys;
        error = "this keyboard has no host keymap";
        return false;
    }
};

// Sound output of a terminal. Devices report what their hardware does; a
// front end's sink turns it into host audio. Calls come from the emulation
// thread with the machine lock held: a sink must not block (queue the event
// for the audio thread).
class AudioSink {
public:
    virtual ~AudioSink() = default;
    // A continuous tone (the alarm) starts or stops. frequency_hz is the
    // pitch the hardware is set to, or 0 while that is not known (the sink
    // then uses a default pitch).
    virtual void tone(bool on, double frequency_hz) = 0;
    // The firmware drives the speaker directly (a gate bit); short pulses
    // of it are clicks.
    virtual void speaker(bool on) = 0;
    // One keyboard click, for hardware that has a dedicated clicker.
    virtual void click() = 0;
};

class NullAudioSink : public AudioSink {
public:
    void tone(bool, double) override {}
    void speaker(bool) override {}
    void click() override {}
};

// Non-volatile storage of a unit (e.g. a setup EEPROM) that the monitor can
// inspect and save.
class NonVolatileStore {
public:
    virtual ~NonVolatileStore() = default;
    // One line: backing file or "ephemeral", and whether unsaved changes exist.
    virtual std::string nv_status() const = 0;
    virtual const std::vector<uint8_t>& nv_contents() const = 0;
    // Write the contents to path (empty: the configured image file).
    virtual bool nv_save(const std::string& path, std::string& error) = 0;
    // Change one byte, as a programmer would with the part out of its
    // socket. Stores that cannot be edited refuse.
    virtual bool nv_set(size_t address, uint8_t value, std::string& error) {
        (void)address;
        (void)value;
        error = "this store cannot be edited";
        return false;
    }
};

// Built-in stand-ins used until a model's real display/keyboard hardware is
// understood.
class PlaceholderDisplay : public DisplaySource {
public:
    PlaceholderDisplay(int width, int height) : width_(width), height_(height) {}
    void render(Framebuffer& fb) override;

private:
    int width_, height_;
};

class LoggingKeyboard : public KeyboardSink {
public:
    void host_key(const HostKeyEvent& ev) override;
};

} // namespace em34xx
