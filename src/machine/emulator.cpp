// SPDX-License-Identifier: GPL-3.0-or-later
#include "machine/emulator.hpp"

#include "machine/model_info.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <chrono>

namespace em34xx {

namespace {
// Instructions per execution slice. Display changes are published and the
// pacing is checked between slices, so this bounds input-to-screen latency
// (2000 instructions are about 7 ms of 3477 time: ~26 cycles each, observed).
constexpr uint64_t kSliceInstructions = 2000;
}

Emulator::Emulator(std::unique_ptr<Machine> machine) : machine_(std::move(machine)) {
    last_stop_.reason = StopEvent::Reason::Reset;
    last_stop_.pc = machine_->cpu().pc_linear();
}

Emulator::~Emulator() { shutdown(); }

void Emulator::start() {
    if (!thread_.joinable()) thread_ = std::thread([this] { thread_main(); });
}

void Emulator::shutdown() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        shutting_down_ = true;
    }
    user_stop_.store(true);
    state_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Emulator::resume(uint64_t max_instructions) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (running_.load()) return;
    user_stop_.store(false);
    run_budget_ = max_instructions;
    run_requested_ = true;
    running_.store(true);
    state_cv_.notify_all();
}

StopEvent Emulator::wait_stopped() {
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_cv_.wait(lock, [&] { return !running_.load(); });
    return last_stop_;
}

bool Emulator::wait_stopped_for(unsigned milliseconds, StopEvent& out) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (!state_cv_.wait_for(lock, std::chrono::milliseconds(milliseconds), [&] { return !running_.load(); }))
        return false;
    out = last_stop_;
    return true;
}

StopEvent Emulator::last_stop() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_stop_;
}

StopEvent Emulator::step(uint64_t n) {
    if (running_.load()) {
        request_stop();
        wait_stopped();
    }
    std::atomic<bool> never{false};
    StopEvent ev;
    {
        std::lock_guard<std::mutex> lock(machine_mutex_);
        ev = machine_->run(n, never);
    }
    if (ev.reason == StopEvent::Reason::None) ev.reason = StopEvent::Reason::StepDone;
    publish_display_change();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_stop_ = ev;
    }
    ++stop_generation_;
    state_cv_.notify_all();
    return ev;
}

void Emulator::finish_run(const StopEvent& ev) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_stop_ = ev;
        run_requested_ = false;
        running_.store(false);
    }
    ++stop_generation_;
    state_cv_.notify_all();
    if (on_stop_) on_stop_(ev);
}

std::unique_ptr<Machine> Emulator::replace_machine(std::unique_ptr<Machine> machine) {
    if (running_.load()) {
        request_stop();
        wait_stopped();
    }
    {
        std::lock_guard<std::mutex> lock(machine_mutex_);
        machine_.swap(machine);
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_stop_ = StopEvent();
        last_stop_.reason = StopEvent::Reason::Reset;
        last_stop_.pc = machine_->cpu().pc_linear();
    }
    publish_display_change();   // a new machine reports its display as changed
    return machine;
}

void Emulator::request_quit(int exit_code) {
    exit_code_.store(exit_code);
    quit_.store(true);
    request_stop();
    state_cv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(display_mutex_);
    }
    display_cv_.notify_all();
}

void Emulator::publish_display_change() {
    bool changed;
    {
        std::lock_guard<std::mutex> lock(machine_mutex_);
        changed = machine_->take_display_change();
    }
    if (!changed) return;
    uint64_t gen;
    std::function<void(uint64_t)> cb;
    {
        std::lock_guard<std::mutex> lock(display_mutex_);
        gen = display_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        cb = on_display_;
    }
    display_cv_.notify_all();
    if (cb) cb(gen);
}

void Emulator::set_display_callback(std::function<void(uint64_t)> cb) {
    std::lock_guard<std::mutex> lock(display_mutex_);
    on_display_ = std::move(cb);
}

uint64_t Emulator::wait_display_change(uint64_t seen, unsigned timeout_ms) {
    std::unique_lock<std::mutex> lock(display_mutex_);
    display_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [&] { return display_generation_.load() != seen || quit_.load(); });
    return display_generation_.load();
}

void Emulator::thread_main() {
    using clock = std::chrono::steady_clock;
    for (;;) {
        uint64_t budget;
        {
            std::unique_lock<std::mutex> lock(state_mutex_);
            state_cv_.wait(lock, [&] { return run_requested_ || shutting_down_; });
            if (shutting_down_) {
                if (running_.load()) {
                    last_stop_.reason = StopEvent::Reason::User;
                    running_.store(false);
                    state_cv_.notify_all();
                }
                return;
            }
            budget = run_budget_;
        }

        const uint64_t clock_hz = machine_->model().clock_hz;
        auto wall_start = clock::now();
        uint64_t cycles_start;
        {
            std::lock_guard<std::mutex> lock(machine_mutex_);
            cycles_start = machine_->cpu().cycles();
        }

        StopEvent ev;
        bool first_slice = true;
        // `--log perf=debug`: achieved speed and pacing sleeps.
        auto report_start = wall_start;
        uint64_t report_cycles = cycles_start;
        double slept_ms = 0, oversleep_ms = 0;
        unsigned sleeps = 0, resyncs = 0;
        for (;;) {
            const uint64_t slice = std::min(budget, kSliceInstructions);
            uint64_t cycles_now;
            uint64_t before, after;
            {
                std::lock_guard<std::mutex> lock(machine_mutex_);
                before = machine_->cpu().instructions();
                ev = machine_->run(slice, user_stop_, first_slice);
                after = machine_->cpu().instructions();
                cycles_now = machine_->cpu().cycles();
            }
            first_slice = false;
            publish_display_change();
            if (ev.reason != StopEvent::Reason::None) break;
            // A full slice ran: count every step, including steps spent
            // halted (HLT does not complete instructions), so that a budget
            // always runs out.
            const uint64_t done = std::max<uint64_t>(after - before, slice);
            budget -= std::min(budget, done);
            if (budget == 0) {
                ev.reason = StopEvent::Reason::StepDone;
                break;
            }
            if (log_enabled("perf", LogLevel::Debug)) {
                const auto now = clock::now();
                const double secs = std::chrono::duration<double>(now - report_start).count();
                if (secs >= 2.0) {
                    LOG_DEBUG("perf",
                              "emulation: %.2f MHz over %.1f s (nominal %.2f MHz, speed %u%%); %u sleeps, "
                              "%.0f ms asleep, %.2f ms average oversleep; %u resyncs",
                              double(cycles_now - report_cycles) / secs / 1e6, secs, double(clock_hz) / 1e6,
                              speed_percent_.load(), sleeps, slept_ms, sleeps ? oversleep_ms / sleeps : 0.0,
                              resyncs);
                    report_start = now;
                    report_cycles = cycles_now;
                    slept_ms = oversleep_ms = 0;
                    sleeps = resyncs = 0;
                }
            }
            // Pace to the model's nominal clock.
            const unsigned pct = speed_percent_.load();
            if (pct != 0 && clock_hz != 0) {
                const double target_s = double(cycles_now - cycles_start) / (double(clock_hz) * pct / 100.0);
                const auto target = wall_start + std::chrono::duration_cast<clock::duration>(
                                                     std::chrono::duration<double>(target_s));
                const auto now = clock::now();
                if (target - now > std::chrono::milliseconds(1)) {
                    // Less than 1 ms ahead: keep running (sleep granularity
                    // is about that coarse, especially under WSL).
                    // Sleep until the target, in short steps so that a stop
                    // request is still seen promptly. One capped sleep per
                    // slice is not enough: when a slice is longer than the
                    // cap the machine runs ahead of its clock (measured: 136%
                    // of nominal in release with 20000-instruction slices).
                    clock::time_point t = now;
                    while (t < target && !user_stop_.load(std::memory_order_relaxed)) {
                        const auto want = std::min<clock::duration>(target - t, std::chrono::milliseconds(20));
                        std::this_thread::sleep_for(want);
                        const auto woke = clock::now();
                        ++sleeps;
                        slept_ms += std::chrono::duration<double, std::milli>(woke - t).count();
                        oversleep_ms += std::chrono::duration<double, std::milli>(woke - t - want).count();
                        t = woke;
                    }
                } else if (now - target > std::chrono::milliseconds(250)) {
                    // Too far behind (e.g. after a debugger pause): resync.
                    wall_start = now;
                    cycles_start = cycles_now;
                    ++resyncs;
                }
            }
        }
        LOG_DEBUG("machine", "stopped: %s at %06x %s", stop_reason_name(ev.reason), ev.pc, ev.detail.c_str());
        finish_run(ev);
    }
}

} // namespace em34xx
