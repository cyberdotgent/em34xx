#include "host/wsc/link_scheduler.hpp"

#include "util/log.hpp"

namespace tn34xx {
namespace wsc {

LinkScheduler::LinkScheduler(const std::vector<StationConfig>& stations, const WscTiming& timing) {
    for (const StationConfig& sc : stations) stations_.push_back(std::make_unique<Station>(sc, timing));
}

Station* LinkScheduler::station(uint8_t address) {
    for (auto& s : stations_)
        if (s->address() == address) return s.get();
    return nullptr;
}

void LinkScheduler::advance(uint64_t emu_ns) {
    if (emu_ns > now_) now_ = emu_ns;
    for (auto& s : stations_) s->pump();
}

bool LinkScheduler::to_station(TwinaxMessage& out) {
    if (outstanding_ >= 0 || stations_.empty()) return false;
    for (size_t k = 0; k < stations_.size(); ++k) {
        const size_t i = (next_ + k) % stations_.size();
        if (!stations_[i]->due(now_)) continue;
        stations_[i]->next_message(now_, out);
        outstanding_ = int(i);
        next_ = i + 1;
        return true;
    }
    return false;
}

void LinkScheduler::from_station(const TwinaxMessage& msg) {
    if (outstanding_ < 0) {
        LOG_DEBUG("wsc", "unsolicited station message ignored: %s", format_twinax_message(msg).c_str());
        return;
    }
    Station& s = *stations_[size_t(outstanding_)];
    outstanding_ = -1;
    s.on_response(msg, now_);
}

void LinkScheduler::no_response() {
    if (outstanding_ < 0) return;
    Station& s = *stations_[size_t(outstanding_)];
    outstanding_ = -1;
    s.on_no_response(now_);
}

std::string LinkScheduler::describe() const {
    std::string out = "twinax: virtual workstation controller\n";
    for (const auto& s : stations_) out += "  " + s->describe();
    return out;
}

} // namespace wsc
} // namespace tn34xx
