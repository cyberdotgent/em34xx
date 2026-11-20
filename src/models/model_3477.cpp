// IBM 3477 InfoWindow.
//
// Primary bring-up target. Facts used here:
//  - Two byte-wide 128 KiB ROMs, 95F4171 and 95F4172, each a linear image
//    (not an even/odd pair). The reset vector lives at the top of 95F4171.
//  - Placement (see roms/ROMS.md): 95F4172 at C0000-DFFFF, 95F4171 at
//    E0000-FFFFF.
// Hypotheses (to be confirmed on hardware):
//  - CPU is 8088-class: two byte-wide ROMs holding contiguous code imply an
//    8-bit data bus.
//  - RAM starts at physical 0; its size is unknown. 64 KiB is mapped as a
//    starting point. Accesses outside it are unexpected and will stop the
//    machine under the default policy, which is how we find the real map.
//  - Peripherals are memory-mapped (no port I/O instruction sequences found).
//  - Clock frequency unknown; 8 MHz is a pacing placeholder only.
#include "models/model.hpp"

#include "machine/machine.hpp"

namespace tn34xx {

namespace {

class Model3477 : public Model {
public:
    Model3477() {
        info_.name = "3477";
        info_.description = "IBM 3477 InfoWindow";
        info_.status = ModelStatus::Bringup;
        info_.cpu = CpuLevel::I8088;
        info_.address_bits = 20;
        info_.clock_hz = 8000000;
        info_.roms = {
            {"system ROM (C0000-DFFFF)", "95f4172.bin", 0x20000,
             "290a58cd5db6543957bdcb2c9e92efefd62a54e37bcd239d41d9bd9cdd6be1d0", 0xC0000},
            {"system ROM (E0000-FFFFF, reset vector)", "95f4171.bin", 0x20000,
             "3f7fb58aec7505315ff94206c8dc05102e319944bd0bde55092274777fb39fde", 0xE0000},
        };
        info_.notes = {
            "CPU 8088 is inferred from byte-wide linear ROM images (hypothesis)",
            "RAM size unknown; 64 KiB at 00000 mapped as a starting point",
            "clock frequency unknown; 8 MHz used for pacing only",
            "one ROM serves monochrome (green/amber/paper-white) and colour units; how the "
            "firmware senses the display type is not yet known",
            "the keyboard ID selects the terminal language; keyboard interface not yet modelled",
            "display and Twinax hardware not yet modelled",
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

const Model& model_3477() {
    static const Model3477 m;
    return m;
}

} // namespace tn34xx
