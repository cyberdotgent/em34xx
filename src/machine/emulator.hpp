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

namespace tn34xx {

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
        std::lock_guard<std::mutex> lock(machine_mutex_);
        return f(*machine_);
    }

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
    std::thread thread_;
};

} // namespace tn34xx
