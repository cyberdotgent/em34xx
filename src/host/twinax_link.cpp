#include "host/twinax_link.hpp"

#include "host/wsc/idle_host.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

namespace tn34xx {

namespace {

unsigned popcount16(uint16_t v) {
    unsigned n = 0;
    for (; v; v &= uint16_t(v - 1)) ++n;
    return n;
}

} // namespace

uint16_t TwinaxFrame::encode() const {
    uint16_t w = 0x8000;  // sync
    for (unsigned i = 0; i < 8; ++i)
        if (data & (1u << i)) w |= uint16_t(1u << (14 - i));   // LSB first
    for (unsigned i = 0; i < 3; ++i)
        if (address & (1u << i)) w |= uint16_t(1u << (6 - i));  // LSB first
    if (popcount16(w) & 1) w |= 0x0008;                       // even parity
    return w;
}

bool TwinaxFrame::decode(uint16_t word, TwinaxFrame& out, bool& parity_ok) {
    if (!(word & 0x8000)) return false;
    uint8_t d = 0;
    for (unsigned i = 0; i < 8; ++i)
        if (word & (1u << (14 - i))) d = uint8_t(d | (1u << i));
    out.data = d;
    uint8_t a = 0;
    for (unsigned i = 0; i < 3; ++i)
        if (word & (1u << (6 - i))) a = uint8_t(a | (1u << i));
    out.address = a;
    parity_ok = (popcount16(uint16_t(word & 0xFFF8u)) & 1) == 0;
    return true;
}

std::string format_twinax_message(const TwinaxMessage& msg) {
    std::string out;
    for (size_t i = 0; i < msg.size(); ++i)
        out += format(i ? " %02x/%u" : "%02x/%u", msg[i].data, msg[i].address);
    return out;
}

void ScriptTwinaxHost::from_station(const TwinaxMessage& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_INFO("twinax", "station -> host: %s", format_twinax_message(msg).c_str());
    received_.push_back(msg);
}

bool ScriptTwinaxHost::to_station(TwinaxMessage& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outgoing_.empty()) return false;
    out = std::move(outgoing_.front());
    outgoing_.pop_front();
    ++sent_count_;
    return true;
}

void ScriptTwinaxHost::no_response() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++no_response_count_;
}

void ScriptTwinaxHost::queue(TwinaxMessage msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    outgoing_.push_back(std::move(msg));
}

std::vector<TwinaxMessage> ScriptTwinaxHost::take_received() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TwinaxMessage> out;
    out.swap(received_);
    return out;
}

std::string ScriptTwinaxHost::describe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return format("twinax: script host, %zu message(s) queued, %llu delivered, %zu response(s) unread, "
                  "%llu without response\n",
                  outgoing_.size(), (unsigned long long)sent_count_, received_.size(),
                  (unsigned long long)no_response_count_);
}

void Tn5250TwinaxHost::from_station(const TwinaxMessage& msg) {
    LOG_DEBUG("tn5250", "dropping station message (bridge not implemented): %s",
              format_twinax_message(msg).c_str());
}

std::string Tn5250TwinaxHost::describe() const {
    return "twinax: tn5250 bridge (not linked in; configuration only)\n" + cfg_.describe();
}

std::unique_ptr<TwinaxHost> make_twinax_host(const TwinaxConfig& twinax, const Tn5250Config& tn5250,
                                             std::string& error, const Tn5250HostFactory& tn5250_factory) {
    error = twinax.validate();
    if (!error.empty()) return nullptr;
    switch (twinax.host) {
    case TwinaxHostKind::None:
        return std::make_unique<NullTwinaxHost>();
    case TwinaxHostKind::Script:
        return std::make_unique<ScriptTwinaxHost>();
    case TwinaxHostKind::Tn5250:
        error = tn5250.validate();
        if (!error.empty()) return nullptr;
        if (!tn5250.configured()) {
            error = "twinax host 'tn5250' needs a tn5250 host name";
            return nullptr;
        }
        if (tn5250_factory) return tn5250_factory(twinax, tn5250, error);
        LOG_WARN("tn5250", "the TN5250 bridge is not linked in; the line will stay idle");
        return std::make_unique<Tn5250TwinaxHost>(tn5250);
    case TwinaxHostKind::Wsc:
        return wsc::make_local_controller(twinax, error);
    }
    error = "unknown twinax host kind";
    return nullptr;
}

} // namespace tn34xx
