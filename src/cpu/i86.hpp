// Interpreter for the Intel 8086 family (8086/8088, 80186/80188; the 80286
// level is reserved for protected-mode support later).
//
// Execution is instruction-granular. Cycle counts are approximate: they are
// deterministic and monotonic, good enough to pace timers, but not a
// bus-cycle-accurate model of the prefetch queue.
#pragma once

#include "cpu/cpu_bus.hpp"

#include <cstdint>
#include <string>

namespace tn34xx {

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

class I86 {
public:
    I86(CpuBus& bus, CpuLevel level);

    CpuLevel level() const { return level_; }
    bool is_186_or_later() const { return level_ >= CpuLevel::I80186; }

    // Reset: CS=FFFF, IP=0000, flags cleared, queue flushed.
    void reset();

    // Execute one instruction (or service one interrupt). Returns the number
    // of clock cycles consumed. A REP string instruction runs to completion
    // unless an interrupt becomes pending, in which case IP is rewound to the
    // prefix so the instruction resumes after the handler returns.
    uint32_t step();

    // External interrupt inputs.
    void set_intr(bool asserted) { intr_ = asserted; }
    void raise_nmi() { nmi_pending_ = true; }

    bool halted() const { return halted_; }
    void set_halted(bool h) { halted_ = h; }

    CpuRegs& regs() { return r_; }
    const CpuRegs& regs() const { return r_; }

    uint64_t cycles() const { return cycles_; }
    uint64_t instructions() const { return instructions_; }

    // Linear address of CS:IP (the next instruction to execute).
    uint32_t pc_linear() const { return linear(r_.seg[CS], r_.ip); }
    uint32_t linear(uint16_t seg, uint16_t off) const {
        return ((uint32_t(seg) << 4) + off) & addr_mask_;
    }
    uint32_t address_mask() const { return addr_mask_; }

    // Flags are stored normalised to what PUSHF would push.
    uint16_t normalise_flags(uint16_t f) const;

private:
    // --- bus helpers -------------------------------------------------
    uint8_t rd8(uint16_t seg, uint16_t off);
    uint16_t rd16(uint16_t seg, uint16_t off);
    void wr8(uint16_t seg, uint16_t off, uint8_t v);
    void wr16(uint16_t seg, uint16_t off, uint16_t v);
    uint8_t fetch8();
    uint16_t fetch16();
    void push(uint16_t v);
    uint16_t pop();
    uint8_t in8(uint16_t port);
    uint16_t in16(uint16_t port);
    void out8(uint16_t port, uint8_t v);
    void out16(uint16_t port, uint16_t v);

    // --- register helpers -------------------------------------------
    uint8_t reg8(unsigned i) const;
    void set_reg8(unsigned i, uint8_t v);
    uint16_t reg(bool w, unsigned i) const { return w ? r_.gpr[i] : reg8(i); }
    void set_reg(bool w, unsigned i, uint16_t v) {
        if (w) r_.gpr[i] = v; else set_reg8(i, uint8_t(v));
    }
    bool flag(uint16_t f) const { return (r_.flags & f) != 0; }
    void set_flag(uint16_t f, bool v) { if (v) r_.flags |= f; else r_.flags &= uint16_t(~f); }

    // --- decoding ----------------------------------------------------
    void decode_modrm();
    uint16_t default_seg(uint16_t seg_if_none) const;
    uint16_t rm_read(bool w);
    void rm_write(bool w, uint16_t v);

    // --- arithmetic --------------------------------------------------
    void set_szp(bool w, uint16_t v);
    uint16_t alu(unsigned op, bool w, uint16_t a, uint16_t b);
    uint16_t inc_dec(bool w, uint16_t v, bool dec);
    uint16_t shift(unsigned op, bool w, uint16_t v, unsigned count);
    void group3(bool w);
    bool divide(bool w, bool is_signed);
    void multiply(bool w, bool is_signed);
    void bcd(uint8_t op);

    // --- control -----------------------------------------------------
    void interrupt(uint8_t vector);
    bool condition(unsigned cc) const;
    void string_op(uint8_t op);
    bool string_iteration(uint8_t op);

    void execute(uint8_t op);
    void execute_186(uint8_t op);
    void invalid_opcode();

    CpuBus& bus_;
    CpuLevel level_;
    uint32_t addr_mask_ = 0xFFFFF;
    bool bus8_ = true;

    CpuRegs r_;
    bool halted_ = false;
    bool intr_ = false;
    bool nmi_pending_ = false;
    bool inhibit_irq_ = false;   // one-instruction shadow after MOV/POP SS

    uint64_t cycles_ = 0;
    uint64_t instructions_ = 0;
    uint32_t step_cycles_ = 0;

    // Per-instruction decode state.
    uint16_t start_ip_ = 0;      // IP of the first prefix byte
    int seg_override_ = -1;
    uint8_t rep_ = 0;            // 0, 0xF2 or 0xF3
    uint8_t modrm_ = 0;
    unsigned mod_ = 0, regf_ = 0, rm_ = 0;
    uint16_t ea_off_ = 0;
    uint16_t ea_seg_ = 0;
    bool rewind_ = false;        // REP interrupted, IP already rewound
};

} // namespace tn34xx
