// A machine: CPU, bus, devices and debug state for one terminal model.
// Not thread-safe; the Emulator serialises access to it.
#pragma once

#include "bus/bus.hpp"
#include "cpu/i86.hpp"
#include "host/twinax_link.hpp"
#include "machine/display_profile.hpp"
#include "machine/peripherals.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tn34xx {

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
    // Configuration strap byte presented by the model, -1 = derived from the
    // display subtype (3477: register 2000A).
    int straps = -1;
    // Setup EEPROM image file; empty = ephemeral (blank, discarded at exit).
    std::string eeprom_path;
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
    I86& cpu() { return cpu_; }

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

    void set_display(DisplaySource* d) { display_ = d; }
    void set_keyboard(KeyboardSink* k) { keyboard_ = k; }
    void set_nvram(NonVolatileStore* n) { nvram_ = n; }
    NonVolatileStore* nvram() const { return nvram_; }
    void set_twinax_host(std::unique_ptr<TwinaxHost> host) { twinax_ = std::move(host); }
    TwinaxHost* twinax_host() const { return twinax_.get(); }

    void render(Framebuffer& fb);
    void key_event(const HostKeyEvent& ev);

private:
    bool breakpoint_hit(uint32_t pc, int& id);

    const ModelInfo& model_;
    Bus bus_;
    I86 cpu_;
    std::vector<std::unique_ptr<Device>> devices_;
    std::vector<Breakpoint> breakpoints_;
    std::vector<uint8_t> bp_bitmap_;   // one bit per linear address
    int next_bp_id_ = 1;

    TerminalOptions options_;
    DisplaySource* display_ = nullptr;
    KeyboardSink* keyboard_ = nullptr;
    NonVolatileStore* nvram_ = nullptr;
    std::unique_ptr<TwinaxHost> twinax_;
    PlaceholderDisplay fallback_display_{640, 400};
    LoggingKeyboard fallback_keyboard_;
};

} // namespace tn34xx
