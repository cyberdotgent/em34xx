// SPDX-License-Identifier: GPL-3.0-or-later
// The link scheduler (layer d): one Twinax cable, several station addresses.
// It plays the controller behind the TwinaxHost interface, in emulated time,
// with at most one message outstanding on the half-duplex line. Stations are
// served round-robin; each one says when it next needs the line (its poll
// cadence, a pending step, or a probe of an absent address).
#pragma once

#include "host/twinax_link.hpp"
#include "host/wsc/station.hpp"

#include <memory>
#include <string>
#include <vector>

namespace tn34xx {
namespace wsc {

class LinkScheduler : public TwinaxHost {
public:
    LinkScheduler(const std::vector<StationConfig>& stations, const WscTiming& timing = WscTiming());

    const char* kind() const override { return "wsc"; }
    void from_station(const TwinaxMessage& msg) override;
    bool to_station(TwinaxMessage& out) override;
    void no_response() override;
    void advance(uint64_t emu_ns) override;
    std::string describe() const override;

    Station* station(uint8_t address);
    const Station* station(uint8_t address) const;
    const std::vector<std::unique_ptr<Station>>& stations() const { return stations_; }
    uint64_t now() const { return now_; }

private:
    std::vector<std::unique_ptr<Station>> stations_;
    uint64_t now_ = 0;
    int outstanding_ = -1;     // index of the station the last message went to
    size_t next_ = 0;          // round-robin start
};

} // namespace wsc
} // namespace tn34xx
