// One station address on the cable: the per-station state machine of the
// virtual workstation controller (layer c, driven by the link scheduler,
// layer d). See docs/twinax-bridge.md.
//
//   Absent --response--> PowerOn --Set Mode--> Identify --reads--> Init --> Online
//      ^                                                                     |
//      +------ no response / busy timeout ------- Recover <-- exception/LPE -+
//
// A station runs one operation at a time: a message out, then the matching
// response (poll, Activate Read) or the link device's no_response() (queue
// loads and Activate Write, which the station does not answer).
#pragma once

#include "host/host_config.hpp"
#include "host/twinax_link.hpp"
#include "host/wsc/keyboard_map.hpp"
#include "host/wsc/screen_writer.hpp"
#include "host/wsc/session_port.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace tn34xx {
namespace wsc {

enum class StationState { Absent, PowerOn, Identify, Init, Online, Recover };
const char* station_state_name(StationState s);

// All in emulated nanoseconds.
struct WscTiming {
    uint64_t idle_poll_ns = 25'000'000;      // per online station
    uint64_t absent_probe_ns = 500'000'000;  // per absent address
    uint64_t busy_repoll_ns = 200'000;       // while waiting for not busy
    uint64_t busy_timeout_ns = 50'000'000;   // then the station is treated as failed
    unsigned no_response_limit = 3;          // consecutive misses before Absent
    size_t printer_chunk = 256;              // SCS bytes per Activate Write [hyp]
};

struct StationCounters {
    uint64_t polls = 0;
    uint64_t messages = 0;
    uint64_t no_responses = 0;
    uint64_t keys = 0;
    uint64_t exceptions = 0;
    uint64_t parity_errors = 0;
    uint64_t discoveries = 0;
    uint64_t steps = 0;
};

class Station {
public:
    Station(const StationConfig& cfg, const WscTiming& timing);

    void attach_display(DisplaySessionPort* port) { display_ = port; }
    void attach_printer(PrinterSessionPort* port) { printer_ = port; }

    // Scheduler side.
    void pump() { if (display_) display_->pump(); }
    bool due(uint64_t now) const;
    void next_message(uint64_t now, TwinaxMessage& out);
    void on_response(const TwinaxMessage& msg, uint64_t now);
    void on_no_response(uint64_t now);

    uint8_t address() const { return uint8_t(cfg_.address); }
    StationType configured_type() const { return cfg_.type; }
    StationState state() const { return state_; }
    const StationIdentity& identity() const { return id_; }
    const DeviceMirror& mirror() const { return mirror_; }
    const StationCounters& counters() const { return counters_; }
    bool type_mismatch() const { return type_mismatch_; }
    std::string describe() const;

private:
    enum class OpKind { Load, WaitReady, ActivateRead, ActivateWrite, Call };
    enum class Expect { Nothing, PollStatus, ReadData };
    struct Op {
        OpKind kind = OpKind::Call;
        std::vector<uint8_t> bytes;   // Load bytes / Activate Write payload
        std::function<void(const std::vector<uint8_t>&)> on_data;   // ActivateRead
        std::function<void()> call;
        int group = 0;                // optional group: an exception skips it
    };

    void enter(StationState s, uint64_t now);
    void lose(uint64_t now, const char* why);
    void begin_discovery(uint8_t first_status, bool two_frames, uint64_t now);
    void queue_identify();
    void queue_init();
    void queue_read(std::vector<uint8_t> load, std::function<void(const std::vector<uint8_t>&)> cb,
                    int group);
    void queue_step(Step step);
    void plan_online_work(uint64_t now);
    void recover(uint8_t status, uint64_t now);
    bool handle_status(uint8_t status, bool two_frames, uint8_t byte2, uint64_t now);
    void handle_byte2(uint8_t level, uint8_t byte2);
    void op_done(uint64_t now);
    FlushOptions flush_options() const;
    bool poll_soon() { const bool p = poll_soon_; poll_soon_ = false; return p; }

    StationConfig cfg_;
    WscTiming timing_;
    DisplaySessionPort* display_ = nullptr;
    PrinterSessionPort* printer_ = nullptr;

    StationState state_ = StationState::Absent;
    StationIdentity id_;
    bool type_mismatch_ = false;
    DeviceMirror mirror_;
    KeyboardMap keymap_;
    StationCounters counters_;

    std::deque<Op> ops_;
    Expect expect_ = Expect::Nothing;
    uint64_t ready_at_ = 0;          // earliest time for the next message
    uint64_t wait_started_ = 0;      // WaitReady start
    bool waiting_ = false;
    bool reset_lpe_ = false;         // next poll resets the line parity error
    unsigned misses_ = 0;            // consecutive no-responses
    int level_ = -1;                 // last response level (-1 = none yet)
    int printer_status_ = -1;
    uint64_t flushed_generation_ = ~uint64_t(0);
    bool announced_ = false;         // device_ready() was sent to the session
    bool poll_soon_ = false;         // a key arrived: poll again without waiting
    StationState resume_state_ = StationState::Online;   // after Recover
};

} // namespace wsc
} // namespace tn34xx
