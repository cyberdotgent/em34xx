#include "machine/emulator.hpp"

#include "models/model.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <chrono>

namespace tn34xx {

namespace {
constexpr uint64_t kSliceInstructions = 20000;
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

void Emulator::request_quit(int exit_code) {
    exit_code_.store(exit_code);
    quit_.store(true);
    request_stop();
    state_cv_.notify_all();
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
            // Pace to the model's nominal clock.
            const unsigned pct = speed_percent_.load();
            if (pct != 0 && clock_hz != 0) {
                const double target_s = double(cycles_now - cycles_start) / (double(clock_hz) * pct / 100.0);
                const auto target = wall_start + std::chrono::duration_cast<clock::duration>(
                                                     std::chrono::duration<double>(target_s));
                const auto now = clock::now();
                if (target > now) {
                    std::this_thread::sleep_for(std::min<clock::duration>(target - now, std::chrono::milliseconds(50)));
                } else if (now - target > std::chrono::milliseconds(250)) {
                    // Too far behind (e.g. after a debugger pause): resync.
                    wall_start = now;
                    cycles_start = cycles_now;
                }
            }
        }
        LOG_DEBUG("machine", "stopped: %s at %06x %s", stop_reason_name(ev.reason), ev.pc, ev.detail.c_str());
        finish_run(ev);
    }
}

} // namespace tn34xx
