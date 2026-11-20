// IBM 3486 InfoWindow II.
//
// Intended first-class target once a complete ROM dump exists. Facts:
//  - One 512 KiB ROM, 54G0011, mapped at 80000-FFFFF (self-referencing
//    pointer tables, see roms/ROMS.md).
//  - The only available dump has chip A17 stuck high: segments 8000, 9000,
//    C000 and D000 are missing, so it is registered as known-bad.
// Hypotheses:
//  - A single byte-wide ROM suggests an 8-bit data bus (8088-class CPU).
//  - RAM at physical 0, size unknown.
#include "models/model.hpp"

#include "machine/machine.hpp"

namespace tn34xx {

namespace {

class Model3486 : public Model {
public:
    Model3486() {
        info_.name = "3486";
        info_.description = "IBM 3486 InfoWindow II";
        info_.status = ModelStatus::RomIncomplete;
        info_.cpu = CpuLevel::I8088;
        info_.address_bits = 20;
        info_.clock_hz = 8000000;
        info_.roms = {
            {"system ROM (80000-FFFFF)", "54g0011.bin", 0x80000, "", 0x80000},
        };
        info_.known_bad_roms = {
            {"54g0011.bin", "f5d968694fdb3b6991b219460eb0ce5eec131797a9c0ea928ec61a6a5b9291ef",
             "read as 27C040 with chip A17 stuck high; segments 8000/9000/C000/D000 missing"},
        };
        info_.notes = {
            "no complete ROM dump yet (re-dump requested)",
            "CPU 8088 is inferred from a single byte-wide ROM (hypothesis)",
            "RAM size unknown; 64 KiB at 00000 mapped as a starting point",
        };
    }

    const ModelInfo& info() const override { return info_; }

    void build(Machine& m, std::vector<LoadedRom>& roms) const override {
        m.bus().map_ram("ram", 0x00000, 0x10000);
        for (LoadedRom& r : roms) m.bus().map_rom(r.spec.file, r.spec.base, std::move(r.data));
    }

private:
    ModelInfo info_;
};

} // namespace

const Model& model_3486() {
    static const Model3486 m;
    return m;
}

} // namespace tn34xx
