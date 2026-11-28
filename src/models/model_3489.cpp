// SPDX-License-Identifier: GPL-3.0-or-later
// IBM 3489 InfoWindow II (modular).
//
// Placeholder. The board carries an 80286; the firmware enters protected
// mode, which the CPU core does not implement yet, and the only ROM dump is
// missing two quarters (U21 A17 not captured). Research notes live in the
// separate 3489emul project.
#include "models/model.hpp"

#include "machine/machine.hpp"

namespace em34xx {

namespace {

class Model3489 : public Model {
public:
    Model3489() {
        info_.name = "3489";
        info_.description = "IBM 3489 InfoWindow II (modular)";
        info_.status = ModelStatus::Placeholder;
        info_.cpu = CpuLevel::I80286;
        info_.address_bits = 24;
        info_.clock_hz = 12000000;
        // 4 display sessions plus 1 printer session (IBM 3489 User's Guide
        // GA18-7492, 1.1.5 and 2.9).
        info_.sessions = {SessionKind::Display, SessionKind::Display, SessionKind::Display, SessionKind::Display,
                          SessionKind::Printer};   // [doc 3489 UG 1.1.5]
        info_.roms = {
            {"system ROM", "IBM-3489.rom", 0x100000, "", 0xF00000},
        };
        info_.known_bad_roms = {
            {"IBM-3489.rom", "d4873793accf0bbbb6ae2dcff1f7f2325c44f05da48dbea3256651faf231af6d",
             "U21 A17 not captured; ROM quarters Q1 and Q3 missing"},
        };
        info_.notes = {
            "four twinax sessions, one of which can be a printer (project requirement; see docs/twinax-bridge.md)",
            "80286 protected mode not implemented in the CPU core",
            "only a defective ROM dump exists",
        };
    }

    const ModelInfo& info() const override { return info_; }

    void build(Machine&, std::vector<LoadedRom>&) const override {}

private:
    ModelInfo info_;
};

} // namespace

const Model& model_3489() {
    static const Model3489 m;
    return m;
}

} // namespace em34xx
