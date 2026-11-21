// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-independent definitions shared by the CPU cores, the machine and the
// debuggers (monitor, GDB stub).
//
// Every core the emulator has, or is expected to get, is an x86-family part:
// the 8088/8086 and 80186/80188 cores (cpu/i86.hpp) and, later, an 80286
// with protected mode for the 3489. The Machine, the monitor and the GDB
// stub use a core only through the Cpu interface below, so a protected-mode
// core slots in by implementing it; see docs/architecture.md ("CPU").
#pragma once

#include "cpu/cpu_bus.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace em34xx {

enum class CpuLevel {
    I8086,
    I8088,
    I80186,
    I80188,
    I80286,
};

const char* cpu_level_name(CpuLevel level);
bool parse_cpu_level(const std::string& text, CpuLevel& out);

// Register indices follow the x86 instruction encoding.
enum Reg16 { AX = 0, CX, DX, BX, SP, BP, SI, DI };
enum SegReg { ES = 0, CS, SS, DS };

enum Flag : uint16_t {
    FLAG_CF = 1u << 0,
    FLAG_PF = 1u << 2,
    FLAG_AF = 1u << 4,
    FLAG_ZF = 1u << 6,
    FLAG_SF = 1u << 7,
    FLAG_TF = 1u << 8,
    FLAG_IF = 1u << 9,
    FLAG_DF = 1u << 10,
    FLAG_OF = 1u << 11,
};

struct CpuRegs {
    uint16_t gpr[8] = {};
    uint16_t seg[4] = {};
    uint16_t ip = 0;
    uint16_t flags = 0;
};

// 80286 system state beyond the real-mode register file: the machine status
// word, the descriptor-table registers and the hidden descriptor caches.
// Only a protected-mode core has it; the debuggers show it when present.
struct DescriptorTableReg {
    uint32_t base = 0;    // 24-bit on the 80286
    uint16_t limit = 0;
};

struct SegmentDescriptorCache {
    uint16_t selector = 0;
    uint32_t base = 0;    // 24-bit on the 80286
    uint16_t limit = 0;
    uint8_t access = 0;   // access-rights byte
};

struct SystemRegs {
    uint16_t msw = 0;                    // PE, MP, EM, TS
    DescriptorTableReg gdtr;
    DescriptorTableReg idtr;
    SegmentDescriptorCache ldtr;
    SegmentDescriptorCache tr;
    SegmentDescriptorCache seg_cache[4]; // ES, CS, SS, DS (index SegReg)
};

// An x86-family CPU core as the machine and the debuggers see it.
class Cpu {
public:
    virtual ~Cpu() = default;

    virtual CpuLevel level() const = 0;
    virtual void reset() = 0;
    // Execute one instruction (or service one interrupt); returns clocks.
    virtual uint32_t step() = 0;

    virtual void set_intr(bool asserted) = 0;
    virtual void raise_nmi() = 0;
    virtual bool halted() const = 0;
    virtual void set_halted(bool h) = 0;

    // The register file common to every level. In protected mode seg[]
    // holds selectors; their descriptors are in system_regs()->seg_cache.
    virtual CpuRegs& regs() = 0;
    virtual const CpuRegs& regs() const = 0;
    virtual uint16_t normalise_flags(uint16_t f) const = 0;

    virtual uint64_t cycles() const = 0;
    virtual uint64_t instructions() const = 0;

    // Linear (= physical: no paging before the 80386) address of the next
    // instruction, and the core's address mask (20 or 24 bits).
    virtual uint32_t pc_linear() const = 0;
    virtual uint32_t address_mask() const = 0;

    // Debugger address translation, without side effects: segment:offset in
    // real mode, selector:offset (through the descriptor tables) in
    // protected mode. False if the selector or offset is not valid.
    virtual bool translate(uint16_t seg, uint32_t off, uint32_t& linear) const = 0;
    // translate() for display purposes: falls back to real-mode arithmetic
    // when the translation is not valid.
    uint32_t debug_linear(uint16_t seg, uint16_t off) const {
        uint32_t l = 0;
        if (translate(seg, off, l)) return l;
        return ((uint32_t(seg) << 4) + off) & address_mask();
    }

    // Protected-mode system state; null for cores without it.
    virtual bool protected_mode() const { return false; }
    virtual const SystemRegs* system_regs() const { return nullptr; }
    virtual SystemRegs* system_regs() { return nullptr; }
};

// The core for a CPU level. Every level currently gets the real-mode I86
// interpreter; an 80286 protected-mode core is created here once it exists.
std::unique_ptr<Cpu> make_cpu(CpuBus& bus, CpuLevel level);

} // namespace em34xx
