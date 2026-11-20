// Interfaces between a machine model and the host front end.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tn34xx {

// 32-bit pixels, 0xAARRGGBB.
struct Framebuffer {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
    // Physical width/height ratio of the visible raster on the real tube.
    // Terminal pixels are generally not square (e.g. 1280x525 shown on a 4:3
    // CRT), so presentation stretches the frame to this ratio. 0 = square.
    double display_aspect = 0.0;

    // Size at which the frame should be presented for a given width.
    int presented_height() const {
        return display_aspect > 0.0 ? int(double(width) / display_aspect + 0.5) : height;
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
};

// A key on the host keyboard, identified by its USB HID usage code (which is
// what SDL scancodes are). Models translate this to their own keyboard.
struct HostKeyEvent {
    uint32_t hid_usage = 0;
    bool pressed = false;
    std::string name;   // for logging only
};

class KeyboardSink {
public:
    virtual ~KeyboardSink() = default;
    virtual void host_key(const HostKeyEvent& ev) = 0;
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

} // namespace tn34xx
