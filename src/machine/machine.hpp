// SPDX-License-Identifier: GPL-3.0-or-later
// A machine: CPU, bus, devices and debug state for one terminal model.
// Not thread-safe; the Emulator serialises access to it.
#pragma once

#include "bus/bus.hpp"
#include "cpu/cpu.hpp"
#include "machine/display_subtype.hpp"
#include "machine/peripherals.hpp"
#include "machine/twinax_port.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace em34xx {

struct ModelInfo;

struct StopEvent {
    enum class Reason {
        None,         // still running (slice limit reached)
        User,         // stop requested by monitor, gdb or Ctrl-C
        Breakpoint,
        Watchpoint,
        BusFault,     // unexpected access under the "stop" policy
        StepDone,     // requested instruction count executed
        Reset,
    };
    Reason reason = Reason::None;
    uint32_t pc = 0;
    int id = 0;               // breakpoint/watchpoint id
    uint32_t data_address = 0; // watchpoint address
    bool data_write = false;
    std::string detail;
};

const char* stop_reason_name(StopEvent::Reason r);

// Per-unit options that do not change the firmware: the same ROM serves
// several display subtypes and keyboard languages. Models present these to
// the firmware through hardware (monitor sense lines, keyboard ID) once that
// hardware is understood.
struct TerminalOptions {
    DisplaySubtype display = DisplaySubtype::Green;
    std::string keyboard_type = "122";   // see src/input/keyboard_ids.hpp
    std::string keyboard_layout = "us";
    int keyboard_id = -1;                // explicit ID byte override, -1 = from layout
    // Enter F13..F24 as Shift+F1..F12 on the host (122-key keyboards only).
    bool shift_fkeys = true;
    // Host keymap: "pcomm" (default), "positional" or a TOML file
    // (src/input/host_keymap.hpp, docs/keyboard.md).
    std::string keymap = "pcomm";
    // Configuration strap byte presented by the model, -1 = derived from the
    // display subtype (3477: register 2000A).
    int straps = -1;
    // Setup EEPROM image file; empty = ephemeral (blank, discarded at exit).
    std::string eeprom_path;
    // Image of the battery-backed RAM; empty = volatile (cleared at power-on).
    std::string nvram_path;
    // 3477: TEMPORARY stand-in for the unmodelled video signature of the
    // power-on display test (see Ibm3477Display, docs/bringup-3477.md).
    bool vdo_shim = true;
};

struct Breakpoint {
    int id = 0;
    uint32_t address = 0;   // linear
    bool temporary = false;
};

class Machine {
public:
    explicit Machine(const ModelInfo& model);
    ~Machine();

    const ModelInfo& model() const { return model_; }
    Bus& bus() { return bus_; }
    Cpu& cpu() { return *cpu_; }
    const Cpu& cpu() const { return *cpu_; }

    template <class T, class... Args>
    T& add_device(Args&&... args) {
        auto dev = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *dev;
        devices_.push_back(std::move(dev));
        return ref;
    }
    const std::vector<std::unique_ptr<Device>>& devices() const { return devices_; }
    Device* find_device(const std::string& name) const;

    void reset();

    // Execute up to max_instructions. Stops early on breakpoints,
    // watchpoints, bus faults or when user_stop becomes true. With
    // skip_initial_breakpoint, a breakpoint at the starting PC is ignored so
    // that "continue" from a breakpoint makes progress.
    StopEvent run(uint64_t max_instructions, const std::atomic<bool>& user_stop,
                  bool skip_initial_breakpoint = true);

    // Called before each instruction when set (used by the "trace" command).
    std::function<void(Machine&)> trace_hook;

    // --- breakpoints ---------------------------------------------------
    int add_breakpoint(uint32_t linear, bool temporary = false);
    bool remove_breakpoint(int id);
    bool remove_breakpoint_at(uint32_t linear);
    void clear_breakpoints();
    const std::vector<Breakpoint>& breakpoints() const { return breakpoints_; }

    // --- peripherals ---------------------------------------------------
    void set_options(const TerminalOptions& o) { options_ = o; }
    const TerminalOptions& options() const { return options_; }

    void set_display(DisplaySource* d) {
        display_ = d;
        display_dirty_ = true;
    }
    void set_keyboard(KeyboardSink* k) { keyboard_ = k; }
    void set_nvram(NonVolatileStore* n) { nvram_ = n; }
    // Where the machine's sound goes; null = nowhere. Devices hold audio(),
    // which forwards to the sink set here, so it can change at any time
    // (under the machine lock).
    void set_audio_sink(AudioSink* s) { audio_.target = s; }
    AudioSink& audio() { return audio_; }
    NonVolatileStore* nvram() const { return nvram_; }
    void set_twinax_host(std::unique_ptr<TwinaxHost> host) { twinax_ = std::move(host); }
    // Replaces the controller on the line and returns the old one, so the
    // caller can destroy it outside the machine lock.
    std::unique_ptr<TwinaxHost> swap_twinax_host(std::unique_ptr<TwinaxHost> host) {
        twinax_.swap(host);
        return host;
    }
    TwinaxHost* twinax_host() const { return twinax_.get(); }

    // The display device's raw frame; present/display_profile.hpp applies
    // the display subtype (render_presented).
    void render(Framebuffer& fb);
    void key_event(const HostKeyEvent& ev);
    // KeyboardSink::terminal_key / set_host_keymap on the model's keyboard;
    // a new keymap is also recorded in options().keymap.
    bool terminal_key(const std::string& function, bool pressed);
    bool set_keymap(const std::string& spec, std::string& error);

    // True once each time the picture may have changed since the last call:
    // a write into the pages the model observes for its display
    // (Bus::observe_writes), a new animation phase, a reset or a new
    // display device. Cheap; the Emulator calls it between slices.
    bool take_display_change();

private:
    bool breakpoint_hit(uint32_t pc, int& id);

    const ModelInfo& model_;
    Bus bus_;
    std::unique_ptr<Cpu> cpu_;
    std::vector<std::unique_ptr<Device>> devices_;
    std::vector<Breakpoint> breakpoints_;
    std::vector<uint8_t> bp_bitmap_;   // one bit per linear address
    int next_bp_id_ = 1;

    TerminalOptions options_;
    DisplaySource* display_ = nullptr;
    KeyboardSink* keyboard_ = nullptr;
    NonVolatileStore* nvram_ = nullptr;
    std::unique_ptr<TwinaxHost> twinax_;
    bool display_dirty_ = true;
    uint64_t seen_display_writes_ = 0;
    uint64_t seen_animation_phase_ = 0;
    struct AudioForwarder : AudioSink {
        AudioSink* target = nullptr;
        void tone(bool on, double hz) override { if (target) target->tone(on, hz); }
        void speaker(bool on) override { if (target) target->speaker(on); }
        void click() override { if (target) target->click(); }
    } audio_;
    PlaceholderDisplay fallback_display_{640, 400};
    LoggingKeyboard fallback_keyboard_;
};

} // namespace em34xx
