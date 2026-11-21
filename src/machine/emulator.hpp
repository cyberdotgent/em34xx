// SPDX-License-Identifier: GPL-3.0-or-later
// Threaded run control around a Machine.
//
// The machine executes on a dedicated thread in slices. Between slices the
// machine lock is released, so the monitor, gdb stub and front end can
// inspect it. All of them talk to the machine only through this class.
#pragma once

#include "machine/machine.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace em34xx {

class Emulator {
public:
    explicit Emulator(std::unique_ptr<Machine> machine);
    ~Emulator();

    void start();
    void shutdown();

    // --- run control -------------------------------------------------
    // Start executing (asynchronously). max_instructions limits the run;
    // when reached the machine stops with reason StepDone.
    void resume(uint64_t max_instructions = UINT64_MAX);
    // Ask a running machine to stop; safe to call from a signal handler.
    void request_stop() { user_stop_.store(true); }
    bool running() const { return running_.load(); }
    // Block until the machine is stopped and return why it stopped.
    StopEvent wait_stopped();
    // As above with a timeout; returns false if still running.
    bool wait_stopped_for(unsigned milliseconds, StopEvent& out);
    // Synchronous single-step of n instructions (machine must be stopped).
    StopEvent step(uint64_t n);
    StopEvent last_stop() const;
    // Called on the emulation thread whenever a run ends (not for step()).
    // Set before start().
    void set_stop_callback(std::function<void(const StopEvent&)> cb) { on_stop_ = std::move(cb); }
    // Incremented every time the machine stops.
    uint64_t stop_generation() const { return stop_generation_.load(); }

    // --- exclusive access -------------------------------------------------
    template <class F>
    auto with_machine(F&& f) -> decltype(f(std::declval<Machine&>())) {
        // Anything done under the lock (a monitor poke, a reset) may change
        // the picture: publish it when the lock is released.
        struct Publish {
            Emulator& e;
            ~Publish() { e.publish_display_change(); }
        } publish{*this};
        std::lock_guard<std::mutex> lock(machine_mutex_);
        return f(*machine_);
    }

    // Put another machine in place of the current one and return the old
    // one (destroy it outside any lock). The machine must be stopped (not
    // running()); the new one is then resumed as usual. Used to rebuild a
    // unit with different hardware options (Application).
    std::unique_ptr<Machine> replace_machine(std::unique_ptr<Machine> machine);

    // --- display change notification -------------------------------------
    // Incremented whenever the machine's picture may have changed (see
    // Machine::take_display_change); checked between execution slices, so
    // at most a few milliseconds late. Front ends redraw only when it moves
    // instead of polling pixels.
    uint64_t display_generation() const { return display_generation_.load(std::memory_order_acquire); }
    // Block until display_generation() differs from `seen`, the emulator is
    // told to quit, or the timeout passes. Returns the current generation.
    uint64_t wait_display_change(uint64_t seen, unsigned timeout_ms);
    // Called with the new generation each time it moves, on whichever thread
    // noticed the change (usually the emulation thread) and without the
    // machine lock held. Keep it short: post an event to the UI thread.
    // May be set or cleared (nullptr) at any time from any thread.
    void set_display_callback(std::function<void(uint64_t)> cb);

    // --- pacing ------------------------------------------------------------
    // Percentage of the model's nominal clock; 0 runs unthrottled.
    void set_speed_percent(unsigned pct) { speed_percent_.store(pct); }
    unsigned speed_percent() const { return speed_percent_.load(); }

    // --- application lifetime ----------------------------------------------
    void request_quit(int exit_code);
    bool quit_requested() const { return quit_.load(); }
    int exit_code() const { return exit_code_.load(); }

private:
    void thread_main();
    void publish_display_change();
    void finish_run(const StopEvent& ev);

    std::unique_ptr<Machine> machine_;
    std::mutex machine_mutex_;

    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    bool run_requested_ = false;
    bool shutting_down_ = false;
    uint64_t run_budget_ = 0;
    StopEvent last_stop_;

    std::atomic<bool> running_{false};
    std::atomic<bool> user_stop_{false};
    std::atomic<uint64_t> stop_generation_{0};
    std::atomic<unsigned> speed_percent_{100};
    std::atomic<bool> quit_{false};
    std::atomic<int> exit_code_{0};

    std::function<void(const StopEvent&)> on_stop_;

    std::atomic<uint64_t> display_generation_{0};
    std::mutex display_mutex_;
    std::condition_variable display_cv_;
    std::function<void(uint64_t)> on_display_;
    std::thread thread_;
};

} // namespace em34xx
