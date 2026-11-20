#include "cpu/i86.hpp"

namespace tn34xx {

namespace {

bool parity8(uint8_t v) {
    v ^= uint8_t(v >> 4);
    v ^= uint8_t(v >> 2);
    v ^= uint8_t(v >> 1);
    return (v & 1) == 0;
}

// Rough base cost per instruction, in clocks, before bus transfers are added.
constexpr uint32_t kBaseCycles = 2;
constexpr uint32_t kBusCycles = 4;

} // namespace

const char* cpu_level_name(CpuLevel level) {
    switch (level) {
    case CpuLevel::I8086: return "8086";
    case CpuLevel::I8088: return "8088";
    case CpuLevel::I80186: return "80186";
    case CpuLevel::I80188: return "80188";
    case CpuLevel::I80286: return "80286";
    }
    return "?";
}

bool parse_cpu_level(const std::string& text, CpuLevel& out) {
    if (text == "8086") out = CpuLevel::I8086;
    else if (text == "8088") out = CpuLevel::I8088;
    else if (text == "80186" || text == "186") out = CpuLevel::I80186;
    else if (text == "80188" || text == "188") out = CpuLevel::I80188;
    else if (text == "80286" || text == "286") out = CpuLevel::I80286;
    else return false;
    return true;
}

I86::I86(CpuBus& bus, CpuLevel level) : bus_(bus), level_(level) {
    addr_mask_ = level == CpuLevel::I80286 ? 0xFFFFFFu : 0xFFFFFu;
    bus8_ = level == CpuLevel::I8088 || level == CpuLevel::I80188;
    reset();
}

uint16_t I86::normalise_flags(uint16_t f) const {
    f = uint16_t((f & 0x0FD5u) | 0x0002u);
    if (level_ < CpuLevel::I80286) f |= 0xF000u;
    return f;
}

void I86::reset() {
    r_ = CpuRegs{};
    r_.seg[CS] = 0xFFFF;
    r_.ip = 0;
    r_.flags = normalise_flags(0);
    halted_ = false;
    nmi_pending_ = false;
    inhibit_irq_ = false;
}

// ---------------------------------------------------------------------------
// Bus helpers

uint8_t I86::rd8(uint16_t seg, uint16_t off) {
    step_cycles_ += kBusCycles;
    return bus_.mem_read8(linear(seg, off));
}

uint16_t I86::rd16(uint16_t seg, uint16_t off) {
    // A word access at offset FFFF wraps to offset 0000 of the same segment.
    uint16_t lo = rd8(seg, off);
    uint16_t hi = rd8(seg, uint16_t(off + 1));
    if (!bus8_) step_cycles_ -= kBusCycles / 2;
    return uint16_t(lo | (hi << 8));
}

void I86::wr8(uint16_t seg, uint16_t off, uint8_t v) {
    step_cycles_ += kBusCycles;
    bus_.mem_write8(linear(seg, off), v);
}

void I86::wr16(uint16_t seg, uint16_t off, uint16_t v) {
    wr8(seg, off, uint8_t(v));
    wr8(seg, uint16_t(off + 1), uint8_t(v >> 8));
    if (!bus8_) step_cycles_ -= kBusCycles / 2;
}

uint8_t I86::fetch8() {
    step_cycles_ += kBusCycles;
    uint8_t v = bus_.fetch8(linear(r_.seg[CS], r_.ip));
    r_.ip = uint16_t(r_.ip + 1);
    return v;
}

uint16_t I86::fetch16() {
    uint16_t lo = fetch8();
    uint16_t hi = fetch8();
    return uint16_t(lo | (hi << 8));
}

void I86::push(uint16_t v) {
    r_.gpr[SP] = uint16_t(r_.gpr[SP] - 2);
    wr16(r_.seg[SS], r_.gpr[SP], v);
}

uint16_t I86::pop() {
    uint16_t v = rd16(r_.seg[SS], r_.gpr[SP]);
    r_.gpr[SP] = uint16_t(r_.gpr[SP] + 2);
    return v;
}

uint8_t I86::in8(uint16_t port) {
    step_cycles_ += kBusCycles;
    return bus_.io_read8(port);
}

uint16_t I86::in16(uint16_t port) {
    uint16_t lo = in8(port);
    uint16_t hi = in8(uint16_t(port + 1));
    return uint16_t(lo | (hi << 8));
}

void I86::out8(uint16_t port, uint8_t v) {
    step_cycles_ += kBusCycles;
    bus_.io_write8(port, v);
}

void I86::out16(uint16_t port, uint16_t v) {
    out8(port, uint8_t(v));
    out8(uint16_t(port + 1), uint8_t(v >> 8));
}

// ---------------------------------------------------------------------------
// Registers

uint8_t I86::reg8(unsigned i) const {
    return i < 4 ? uint8_t(r_.gpr[i]) : uint8_t(r_.gpr[i - 4] >> 8);
}

void I86::set_reg8(unsigned i, uint8_t v) {
    if (i < 4) r_.gpr[i] = uint16_t((r_.gpr[i] & 0xFF00u) | v);
    else r_.gpr[i - 4] = uint16_t((r_.gpr[i - 4] & 0x00FFu) | (uint16_t(v) << 8));
}

// ---------------------------------------------------------------------------
// ModR/M decoding

uint16_t I86::default_seg(uint16_t seg_if_none) const {
    return seg_override_ >= 0 ? r_.seg[seg_override_] : seg_if_none;
}

void I86::decode_modrm() {
    modrm_ = fetch8();
    mod_ = modrm_ >> 6;
    regf_ = (modrm_ >> 3) & 7;
    rm_ = modrm_ & 7;
    if (mod_ == 3) return;

    uint16_t off = 0;
    unsigned seg = DS;
    switch (rm_) {
    case 0: off = uint16_t(r_.gpr[BX] + r_.gpr[SI]); break;
    case 1: off = uint16_t(r_.gpr[BX] + r_.gpr[DI]); break;
    case 2: off = uint16_t(r_.gpr[BP] + r_.gpr[SI]); seg = SS; break;
    case 3: off = uint16_t(r_.gpr[BP] + r_.gpr[DI]); seg = SS; break;
    case 4: off = r_.gpr[SI]; break;
    case 5: off = r_.gpr[DI]; break;
    case 6:
        if (mod_ == 0) off = fetch16();
        else { off = r_.gpr[BP]; seg = SS; }
        break;
    case 7: off = r_.gpr[BX]; break;
    }
    if (mod_ == 1) off = uint16_t(off + int16_t(int8_t(fetch8())));
    else if (mod_ == 2) off = uint16_t(off + fetch16());
    ea_off_ = off;
    ea_seg_ = default_seg(r_.seg[seg]);
    step_cycles_ += 5;
}

uint16_t I86::rm_read(bool w) {
    if (mod_ == 3) return reg(w, rm_);
    return w ? rd16(ea_seg_, ea_off_) : rd8(ea_seg_, ea_off_);
}

void I86::rm_write(bool w, uint16_t v) {
    if (mod_ == 3) { set_reg(w, rm_, v); return; }
    if (w) wr16(ea_seg_, ea_off_, v);
    else wr8(ea_seg_, ea_off_, uint8_t(v));
}

// ---------------------------------------------------------------------------
// Arithmetic

void I86::set_szp(bool w, uint16_t v) {
    uint16_t sign = w ? 0x8000 : 0x80;
    uint16_t mask = w ? 0xFFFF : 0xFF;
    set_flag(FLAG_ZF, (v & mask) == 0);
    set_flag(FLAG_SF, (v & sign) != 0);
    set_flag(FLAG_PF, parity8(uint8_t(v)));
}

// op: 0 ADD, 1 OR, 2 ADC, 3 SBB, 4 AND, 5 SUB, 6 XOR, 7 CMP
uint16_t I86::alu(unsigned op, bool w, uint16_t a, uint16_t b) {
    const uint32_t mask = w ? 0xFFFFu : 0xFFu;
    const uint32_t sign = w ? 0x8000u : 0x80u;
    uint32_t res = 0;
    switch (op) {
    case 0:
    case 2: {
        uint32_t c = (op == 2 && flag(FLAG_CF)) ? 1 : 0;
        res = uint32_t(a) + b + c;
        set_flag(FLAG_CF, res > mask);
        set_flag(FLAG_OF, ((a ^ res) & (b ^ res) & sign) != 0);
        set_flag(FLAG_AF, ((a ^ b ^ res) & 0x10) != 0);
        break;
    }
    case 3:
    case 5:
    case 7: {
        uint32_t c = (op == 3 && flag(FLAG_CF)) ? 1 : 0;
        res = uint32_t(a) - b - c;
        set_flag(FLAG_CF, (uint32_t(b) + c) > a);
        set_flag(FLAG_OF, ((a ^ b) & (a ^ res) & sign) != 0);
        set_flag(FLAG_AF, ((a ^ b ^ res) & 0x10) != 0);
        break;
    }
    case 1: res = uint32_t(a | b); goto logic;
    case 4: res = uint32_t(a & b); goto logic;
    case 6: res = uint32_t(a ^ b);
    logic:
        set_flag(FLAG_CF, false);
        set_flag(FLAG_OF, false);
        set_flag(FLAG_AF, false);
        break;
    }
    res &= mask;
    set_szp(w, uint16_t(res));
    return uint16_t(res);
}

uint16_t I86::inc_dec(bool w, uint16_t v, bool dec) {
    bool cf = flag(FLAG_CF);
    uint16_t r = alu(dec ? 5 : 0, w, v, 1);
    set_flag(FLAG_CF, cf);
    return r;
}

// op: 0 ROL, 1 ROR, 2 RCL, 3 RCR, 4 SHL, 5 SHR, 6 SETMO (8086) / SHL, 7 SAR
uint16_t I86::shift(unsigned op, bool w, uint16_t v, unsigned count) {
    const uint32_t mask = w ? 0xFFFFu : 0xFFu;
    const uint32_t sign = w ? 0x8000u : 0x80u;
    if (is_186_or_later()) count &= 0x1F;
    if (count == 0) return v;
    step_cycles_ += 4 * count;

    uint32_t x = v & mask;
    bool cf = flag(FLAG_CF);

    if (op == 6 && !is_186_or_later()) {
        // Undocumented SETMO/SETMOC: result is all ones.
        x = mask;
        set_flag(FLAG_CF, false);
        set_flag(FLAG_OF, false);
        set_flag(FLAG_AF, false);
        set_szp(w, uint16_t(x));
        return uint16_t(x);
    }
    if (op == 6) op = 4;

    for (unsigned i = 0; i < count; ++i) {
        switch (op) {
        case 0: cf = (x & sign) != 0; x = ((x << 1) | (cf ? 1 : 0)) & mask; break;
        case 1: cf = (x & 1) != 0; x = (x >> 1) | (cf ? sign : 0); break;
        case 2: { bool out = (x & sign) != 0; x = ((x << 1) | (cf ? 1 : 0)) & mask; cf = out; break; }
        case 3: { bool out = (x & 1) != 0; x = (x >> 1) | (cf ? sign : 0); cf = out; break; }
        case 4: cf = (x & sign) != 0; x = (x << 1) & mask; break;
        case 5: cf = (x & 1) != 0; x >>= 1; break;
        case 7: cf = (x & 1) != 0; x = (x >> 1) | (x & sign); break;
        }
    }
    set_flag(FLAG_CF, cf);
    const bool msb = (x & sign) != 0;
    switch (op) {
    case 0: case 2: case 4:
        set_flag(FLAG_OF, msb != cf);
        break;
    case 1: case 3: case 5:
        // OF reflects the sign change of the final single-bit step: the two
        // most significant bits of the result differ.
        set_flag(FLAG_OF, ((x ^ (x << 1)) & sign) != 0);
        break;
    case 7:
        set_flag(FLAG_OF, false);
        break;
    }
    if (op >= 4) {
        set_szp(w, uint16_t(x));
        set_flag(FLAG_AF, false);
    }
    return uint16_t(x);
}

void I86::multiply(bool w, bool is_signed) {
    uint16_t src = rm_read(w);
    const bool negate = rep_ != 0 && is_signed && !is_186_or_later();
    step_cycles_ += w ? 118 : 70;
    if (!w) {
        uint8_t al = uint8_t(r_.gpr[AX]);
        uint16_t res;
        bool overflow;
        if (is_signed) {
            int32_t p = int32_t(int8_t(al)) * int32_t(int8_t(uint8_t(src)));
            if (negate) p = -p;
            res = uint16_t(p);
            overflow = int32_t(int8_t(uint8_t(res))) != int32_t(int16_t(res));
        } else {
            res = uint16_t(al * uint8_t(src));
            overflow = (res & 0xFF00) != 0;
        }
        r_.gpr[AX] = res;
        set_flag(FLAG_CF, overflow);
        set_flag(FLAG_OF, overflow);
        set_szp(false, uint16_t(res >> 8));
    } else {
        uint32_t res;
        bool overflow;
        if (is_signed) {
            int32_t p = int32_t(int16_t(r_.gpr[AX])) * int32_t(int16_t(src));
            if (negate) p = -p;
            res = uint32_t(p);
            overflow = int32_t(int16_t(uint16_t(res))) != p;
        } else {
            res = uint32_t(r_.gpr[AX]) * src;
            overflow = (res & 0xFFFF0000u) != 0;
        }
        r_.gpr[AX] = uint16_t(res);
        r_.gpr[DX] = uint16_t(res >> 16);
        set_flag(FLAG_CF, overflow);
        set_flag(FLAG_OF, overflow);
        set_szp(true, uint16_t(res >> 16));
    }
}

// Returns false if a divide error was raised.
bool I86::divide(bool w, bool is_signed) {
    uint16_t src = rm_read(w);
    const bool negate = rep_ != 0 && is_signed && !is_186_or_later();
    step_cycles_ += w ? 150 : 85;
    if (!w) {
        uint16_t dividend = r_.gpr[AX];
        uint8_t divisor = uint8_t(src);
        if (divisor == 0) { interrupt(0); return false; }
        if (is_signed) {
            int32_t a = int16_t(dividend);
            int32_t b = int8_t(divisor);
            int32_t q = a / b;
            int32_t rem = a % b;
            if (negate) q = -q;
            const int32_t qmin = is_186_or_later() ? -128 : -127;
            if (q > 127 || q < qmin) { interrupt(0); return false; }
            r_.gpr[AX] = uint16_t((uint8_t(rem) << 8) | uint8_t(q));
        } else {
            uint32_t q = dividend / divisor;
            uint32_t rem = dividend % divisor;
            if (q > 0xFF) { interrupt(0); return false; }
            r_.gpr[AX] = uint16_t((rem << 8) | q);
        }
    } else {
        uint32_t dividend = (uint32_t(r_.gpr[DX]) << 16) | r_.gpr[AX];
        uint16_t divisor = src;
        if (divisor == 0) { interrupt(0); return false; }
        if (is_signed) {
            int64_t a = int32_t(dividend);
            int64_t b = int16_t(divisor);
            int64_t q = a / b;
            int64_t rem = a % b;
            if (negate) q = -q;
            const int64_t qmin = is_186_or_later() ? -32768 : -32767;
            if (q > 32767 || q < qmin) { interrupt(0); return false; }
            r_.gpr[AX] = uint16_t(q);
            r_.gpr[DX] = uint16_t(rem);
        } else {
            uint32_t q = dividend / divisor;
            uint32_t rem = dividend % divisor;
            if (q > 0xFFFF) { interrupt(0); return false; }
            r_.gpr[AX] = uint16_t(q);
            r_.gpr[DX] = uint16_t(rem);
        }
    }
    return true;
}

void I86::group3(bool w) {
    switch (regf_) {
    case 0:
    case 1: { // TEST rm, imm (/1 is an undocumented alias)
        uint16_t a = rm_read(w);
        uint16_t b = w ? fetch16() : fetch8();
        alu(4, w, a, b);
        break;
    }
    case 2: rm_write(w, uint16_t(~rm_read(w))); break;
    case 3: {
        uint16_t v = rm_read(w);
        uint16_t res = alu(5, w, 0, v);
        rm_write(w, res);
        break;
    }
    case 4: multiply(w, false); break;
    case 5: multiply(w, true); break;
    case 6: divide(w, false); break;
    case 7: divide(w, true); break;
    }
}

void I86::bcd(uint8_t op) {
    uint8_t al = uint8_t(r_.gpr[AX]);
    switch (op) {
    case 0x27: { // DAA
        const uint8_t old_al = al;
        const bool old_cf = flag(FLAG_CF);
        const bool old_af = flag(FLAG_AF);
        bool cf = false;
        if ((al & 0x0F) > 9 || old_af) {
            al = uint8_t(al + 6);
            set_flag(FLAG_AF, true);
        } else {
            set_flag(FLAG_AF, false);
        }
        if (old_al > (old_af ? 0x9F : 0x99) || old_cf) {
            al = uint8_t(al + 0x60);
            cf = true;
        }
        set_flag(FLAG_CF, cf);
        set_flag(FLAG_OF, false);
        set_szp(false, al);
        set_reg8(0, al);
        break;
    }
    case 0x2F: { // DAS
        const uint8_t old_al = al;
        const bool old_cf = flag(FLAG_CF);
        const bool old_af = flag(FLAG_AF);
        bool cf = false;
        if ((al & 0x0F) > 9 || old_af) {
            al = uint8_t(al - 6);
            set_flag(FLAG_AF, true);
        } else {
            set_flag(FLAG_AF, false);
        }
        if (old_al > (old_af ? 0x9F : 0x99) || old_cf) {
            al = uint8_t(al - 0x60);
            cf = true;
        }
        set_flag(FLAG_CF, cf);
        set_flag(FLAG_OF, false);
        set_szp(false, al);
        set_reg8(0, al);
        break;
    }
    case 0x37: // AAA
    case 0x3F: { // AAS
        const bool adjust = (al & 0x0F) > 9 || flag(FLAG_AF);
        if (adjust) {
            uint8_t ah = uint8_t(r_.gpr[AX] >> 8);
            if (op == 0x37) { al = uint8_t(al + 6); ah = uint8_t(ah + 1); }
            else { al = uint8_t(al - 6); ah = uint8_t(ah - 1); }
            set_reg8(4, ah);
        }
        set_flag(FLAG_AF, adjust);
        set_flag(FLAG_CF, adjust);
        set_reg8(0, uint8_t(al & 0x0F));
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Control flow

void I86::interrupt(uint8_t vector) {
    step_cycles_ += 20;
    push(normalise_flags(r_.flags));
    set_flag(FLAG_IF, false);
    set_flag(FLAG_TF, false);
    push(r_.seg[CS]);
    push(r_.ip);
    uint16_t ip = rd16(0, uint16_t(vector * 4));
    uint16_t cs = rd16(0, uint16_t(vector * 4 + 2));
    r_.ip = ip;
    r_.seg[CS] = cs;
    halted_ = false;
}

bool I86::condition(unsigned cc) const {
    const bool cf = flag(FLAG_CF), zf = flag(FLAG_ZF), sf = flag(FLAG_SF);
    const bool of = flag(FLAG_OF), pf = flag(FLAG_PF);
    bool r = false;
    switch (cc >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf || zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf != of; break;
    case 7: r = zf || (sf != of); break;
    }
    return (cc & 1) ? !r : r;
}

void I86::invalid_opcode() {
    // The 80186 and later raise #UD (vector 6) with the faulting IP.
    r_.ip = start_ip_;
    interrupt(6);
}

// One iteration of a string instruction. Returns false when a REPE/REPNE
// comparison terminates the repetition.
bool I86::string_iteration(uint8_t op) {
    const bool w = (op & 1) != 0;
    const int16_t delta = int16_t((w ? 2 : 1) * (flag(FLAG_DF) ? -1 : 1));
    const uint16_t src_seg = default_seg(r_.seg[DS]);
    uint16_t& si = r_.gpr[SI];
    uint16_t& di = r_.gpr[DI];
    switch (op) {
    case 0x6C: case 0x6D: { // INS
        uint16_t v = w ? in16(r_.gpr[DX]) : in8(r_.gpr[DX]);
        if (w) wr16(r_.seg[ES], di, v); else wr8(r_.seg[ES], di, uint8_t(v));
        di = uint16_t(di + delta);
        return true;
    }
    case 0x6E: case 0x6F: { // OUTS
        uint16_t v = w ? rd16(src_seg, si) : rd8(src_seg, si);
        if (w) out16(r_.gpr[DX], v); else out8(r_.gpr[DX], uint8_t(v));
        si = uint16_t(si + delta);
        return true;
    }
    case 0xA4: case 0xA5: { // MOVS
        uint16_t v = w ? rd16(src_seg, si) : rd8(src_seg, si);
        if (w) wr16(r_.seg[ES], di, v); else wr8(r_.seg[ES], di, uint8_t(v));
        si = uint16_t(si + delta);
        di = uint16_t(di + delta);
        return true;
    }
    case 0xA6: case 0xA7: { // CMPS
        uint16_t a = w ? rd16(src_seg, si) : rd8(src_seg, si);
        uint16_t b = w ? rd16(r_.seg[ES], di) : rd8(r_.seg[ES], di);
        alu(7, w, a, b);
        si = uint16_t(si + delta);
        di = uint16_t(di + delta);
        break;
    }
    case 0xAA: case 0xAB: // STOS
        if (w) wr16(r_.seg[ES], di, r_.gpr[AX]);
        else wr8(r_.seg[ES], di, uint8_t(r_.gpr[AX]));
        di = uint16_t(di + delta);
        return true;
    case 0xAC: case 0xAD: // LODS
        if (w) r_.gpr[AX] = rd16(src_seg, si);
        else set_reg8(0, rd8(src_seg, si));
        si = uint16_t(si + delta);
        return true;
    case 0xAE: case 0xAF: { // SCAS
        uint16_t b = w ? rd16(r_.seg[ES], di) : rd8(r_.seg[ES], di);
        alu(7, w, w ? r_.gpr[AX] : uint16_t(r_.gpr[AX] & 0xFF), b);
        di = uint16_t(di + delta);
        break;
    }
    }
    // Comparison instructions only.
    if (rep_ == 0xF3) return flag(FLAG_ZF);
    if (rep_ == 0xF2) return !flag(FLAG_ZF);
    return true;
}

void I86::string_op(uint8_t op) {
    if (rep_ == 0) {
        string_iteration(op);
        return;
    }
    while (r_.gpr[CX] != 0) {
        bool keep_going = string_iteration(op);
        r_.gpr[CX] = uint16_t(r_.gpr[CX] - 1);
        if (!keep_going || r_.gpr[CX] == 0) break;
        if ((intr_ && flag(FLAG_IF)) || nmi_pending_) {
            // Resume the instruction after the interrupt is serviced.
            r_.ip = start_ip_;
            rewind_ = true;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Main loop

uint32_t I86::step() {
    step_cycles_ = 0;

    // Interrupt recognition happens between instructions.
    if (nmi_pending_) {
        nmi_pending_ = false;
        interrupt(2);
    } else if (intr_ && flag(FLAG_IF) && !inhibit_irq_) {
        uint8_t vector = bus_.interrupt_ack();
        interrupt(vector);
    } else if (halted_) {
        step_cycles_ = kBusCycles;
        cycles_ += step_cycles_;
        return step_cycles_;
    }
    inhibit_irq_ = false;

    const bool trap = flag(FLAG_TF);
    start_ip_ = r_.ip;
    seg_override_ = -1;
    rep_ = 0;
    rewind_ = false;

    // Prefixes.
    uint8_t op;
    for (;;) {
        op = fetch8();
        if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) {
            seg_override_ = (op >> 3) & 3;
        } else if (op == 0xF2 || op == 0xF3) {
            rep_ = op;
        } else if (op == 0xF0 || (op == 0xF1 && !is_186_or_later())) {
            // LOCK: no effect on a single-processor bus.
        } else {
            break;
        }
    }

    execute(op);

    step_cycles_ += kBaseCycles;
    ++instructions_;
    if (trap && !rewind_) interrupt(1);
    cycles_ += step_cycles_;
    return step_cycles_;
}

void I86::execute(uint8_t op) {
    // ALU block: 00-3F with low 3 bits 0..5.
    if (op < 0x40 && (op & 7) < 6) {
        const unsigned aop = op >> 3;
        const bool w = (op & 1) != 0;
        switch (op & 7) {
        case 0: case 1: { // rm, reg
            decode_modrm();
            uint16_t res = alu(aop, w, rm_read(w), reg(w, regf_));
            if (aop != 7) rm_write(w, res);
            break;
        }
        case 2: case 3: { // reg, rm
            decode_modrm();
            uint16_t res = alu(aop, w, reg(w, regf_), rm_read(w));
            if (aop != 7) set_reg(w, regf_, res);
            break;
        }
        case 4: { // AL, imm8
            uint16_t res = alu(aop, false, uint8_t(r_.gpr[AX]), fetch8());
            if (aop != 7) set_reg8(0, uint8_t(res));
            break;
        }
        case 5: { // AX, imm16
            uint16_t res = alu(aop, true, r_.gpr[AX], fetch16());
            if (aop != 7) r_.gpr[AX] = res;
            break;
        }
        }
        return;
    }

    if (is_186_or_later() && ((op >= 0x60 && op <= 0x6F) || op == 0xC0 || op == 0xC1 ||
                              op == 0xC8 || op == 0xC9)) {
        execute_186(op);
        return;
    }

    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E: // PUSH seg
        push(r_.seg[(op >> 3) & 3]);
        break;
    case 0x07: case 0x17: case 0x1F: // POP seg
        r_.seg[(op >> 3) & 3] = pop();
        if (op == 0x17) inhibit_irq_ = true;
        break;
    case 0x0F: // POP CS (8086 only)
        if (is_186_or_later()) invalid_opcode();
        else r_.seg[CS] = pop();
        break;
    case 0x27: case 0x2F: case 0x37: case 0x3F:
        bcd(op);
        break;

    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        r_.gpr[op & 7] = inc_dec(true, r_.gpr[op & 7], false);
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        r_.gpr[op & 7] = inc_dec(true, r_.gpr[op & 7], true);
        break;
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x55: case 0x56: case 0x57:
        push(r_.gpr[op & 7]);
        break;
    case 0x54: // PUSH SP: the 8086 pushes the already-decremented value.
        if (level_ >= CpuLevel::I80286) {
            push(r_.gpr[SP]);
        } else {
            r_.gpr[SP] = uint16_t(r_.gpr[SP] - 2);
            wr16(r_.seg[SS], r_.gpr[SP], r_.gpr[SP]);
        }
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint16_t v = pop();
        r_.gpr[op & 7] = v;
        break;
    }

    // 60-6F alias the conditional jumps on the 8086/8088.
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t disp = int8_t(fetch8());
        if (condition(op & 0x0F)) {
            r_.ip = uint16_t(r_.ip + disp);
            step_cycles_ += 12;
        }
        break;
    }

    case 0x80: case 0x81: case 0x82: case 0x83: { // group 1
        const bool w = (op & 1) != 0;
        decode_modrm();
        uint16_t a = rm_read(w);
        uint16_t b;
        if (op == 0x81) b = fetch16();
        else if (op == 0x83) b = uint16_t(int16_t(int8_t(fetch8())));
        else b = fetch8();
        uint16_t res = alu(regf_, w, a, b);
        if (regf_ != 7) rm_write(w, res);
        break;
    }
    case 0x84: case 0x85: { // TEST rm, reg
        const bool w = (op & 1) != 0;
        decode_modrm();
        alu(4, w, rm_read(w), reg(w, regf_));
        break;
    }
    case 0x86: case 0x87: { // XCHG rm, reg
        const bool w = (op & 1) != 0;
        decode_modrm();
        uint16_t a = rm_read(w);
        uint16_t b = reg(w, regf_);
        set_reg(w, regf_, a);
        rm_write(w, b);
        break;
    }
    case 0x88: case 0x89: { // MOV rm, reg
        const bool w = (op & 1) != 0;
        decode_modrm();
        rm_write(w, reg(w, regf_));
        break;
    }
    case 0x8A: case 0x8B: { // MOV reg, rm
        const bool w = (op & 1) != 0;
        decode_modrm();
        set_reg(w, regf_, rm_read(w));
        break;
    }
    case 0x8C: // MOV rm, sreg
        decode_modrm();
        rm_write(true, r_.seg[regf_ & 3]);
        break;
    case 0x8D: // LEA
        decode_modrm();
        if (mod_ == 3) {
            // Undefined; leave the register untouched.
        } else {
            r_.gpr[regf_] = ea_off_;
        }
        break;
    case 0x8E: { // MOV sreg, rm
        decode_modrm();
        uint16_t v = rm_read(true);
        r_.seg[regf_ & 3] = v;
        if ((regf_ & 3) == SS) inhibit_irq_ = true;
        break;
    }
    case 0x8F: { // POP rm
        decode_modrm();
        // The stack pointer is incremented before the destination address is
        // used, but the EA was already computed during decode.
        uint16_t v = pop();
        rm_write(true, v);
        break;
    }

    case 0x90: break; // NOP (XCHG AX, AX)
    case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97: {
        uint16_t t = r_.gpr[op & 7];
        r_.gpr[op & 7] = r_.gpr[AX];
        r_.gpr[AX] = t;
        break;
    }
    case 0x98: // CBW
        r_.gpr[AX] = uint16_t(int16_t(int8_t(uint8_t(r_.gpr[AX]))));
        break;
    case 0x99: // CWD
        r_.gpr[DX] = (r_.gpr[AX] & 0x8000) ? 0xFFFF : 0x0000;
        break;
    case 0x9A: { // CALL far
        uint16_t ip = fetch16();
        uint16_t cs = fetch16();
        push(r_.seg[CS]);
        push(r_.ip);
        r_.seg[CS] = cs;
        r_.ip = ip;
        break;
    }
    case 0x9B: break; // WAIT: no coprocessor, TEST is never asserted
    case 0x9C: push(normalise_flags(r_.flags)); break;
    case 0x9D: r_.flags = normalise_flags(pop()); break;
    case 0x9E: // SAHF
        r_.flags = normalise_flags(uint16_t((r_.flags & 0xFF00) | (r_.gpr[AX] >> 8)));
        break;
    case 0x9F: // LAHF
        set_reg8(4, uint8_t(normalise_flags(r_.flags)));
        break;

    case 0xA0: case 0xA1: { // MOV AL/AX, moffs
        uint16_t off = fetch16();
        uint16_t seg = default_seg(r_.seg[DS]);
        if (op & 1) r_.gpr[AX] = rd16(seg, off);
        else set_reg8(0, rd8(seg, off));
        break;
    }
    case 0xA2: case 0xA3: { // MOV moffs, AL/AX
        uint16_t off = fetch16();
        uint16_t seg = default_seg(r_.seg[DS]);
        if (op & 1) wr16(seg, off, r_.gpr[AX]);
        else wr8(seg, off, uint8_t(r_.gpr[AX]));
        break;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xAA: case 0xAB: case 0xAC: case 0xAD:
    case 0xAE: case 0xAF:
        string_op(op);
        break;
    case 0xA8: alu(4, false, uint8_t(r_.gpr[AX]), fetch8()); break;
    case 0xA9: alu(4, true, r_.gpr[AX], fetch16()); break;

    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        set_reg8(op & 7, fetch8());
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        r_.gpr[op & 7] = fetch16();
        break;

    case 0xC0: case 0xC2: { // RET imm16 (C0 is an 8086 alias)
        uint16_t n = fetch16();
        r_.ip = pop();
        r_.gpr[SP] = uint16_t(r_.gpr[SP] + n);
        break;
    }
    case 0xC1: case 0xC3: // RET (C1 is an 8086 alias)
        r_.ip = pop();
        break;
    case 0xC4: case 0xC5: { // LES / LDS
        decode_modrm();
        // With a register operand the 8086 uses the last computed EA; we
        // mirror that by reusing ea_off_/ea_seg_ from this decode.
        uint16_t off = rd16(ea_seg_, ea_off_);
        uint16_t seg = rd16(ea_seg_, uint16_t(ea_off_ + 2));
        r_.gpr[regf_] = off;
        r_.seg[op == 0xC4 ? ES : DS] = seg;
        break;
    }
    case 0xC6: case 0xC7: { // MOV rm, imm
        const bool w = (op & 1) != 0;
        decode_modrm();
        uint16_t v = w ? fetch16() : fetch8();
        rm_write(w, v);
        break;
    }
    case 0xC8: case 0xCA: { // RETF imm16 (C8 is an 8086 alias)
        uint16_t n = fetch16();
        r_.ip = pop();
        r_.seg[CS] = pop();
        r_.gpr[SP] = uint16_t(r_.gpr[SP] + n);
        break;
    }
    case 0xC9: case 0xCB: // RETF (C9 is an 8086 alias)
        r_.ip = pop();
        r_.seg[CS] = pop();
        break;
    case 0xCC: interrupt(3); break;
    case 0xCD: interrupt(fetch8()); break;
    case 0xCE: if (flag(FLAG_OF)) interrupt(4); break;
    case 0xCF: // IRET
        r_.ip = pop();
        r_.seg[CS] = pop();
        r_.flags = normalise_flags(pop());
        break;

    case 0xD0: case 0xD1: case 0xD2: case 0xD3: { // shift group
        const bool w = (op & 1) != 0;
        decode_modrm();
        unsigned count = (op & 2) ? (r_.gpr[CX] & 0xFF) : 1;
        uint16_t v = rm_read(w);
        uint16_t res = shift(regf_, w, v, count);
        if (count != 0) rm_write(w, res);
        break;
    }
    case 0xD4: { // AAM
        uint8_t base = fetch8();
        if (base == 0) {
            // The 8086 raises the divide error with flags from a zero result.
            set_flag(FLAG_CF, false);
            set_flag(FLAG_OF, false);
            set_flag(FLAG_AF, false);
            set_szp(false, 0);
            interrupt(0);
            break;
        }
        uint8_t al = uint8_t(r_.gpr[AX]);
        uint8_t ah = uint8_t(al / base);
        al = uint8_t(al % base);
        r_.gpr[AX] = uint16_t((ah << 8) | al);
        set_szp(false, al);
        step_cycles_ += 80;
        break;
    }
    case 0xD5: { // AAD
        uint8_t base = fetch8();
        uint8_t al = uint8_t(r_.gpr[AX]);
        uint8_t ah = uint8_t(r_.gpr[AX] >> 8);
        uint8_t res = uint8_t(alu(0, false, al, uint8_t(ah * base)));
        r_.gpr[AX] = res;
        step_cycles_ += 60;
        break;
    }
    case 0xD6: // SALC (undocumented)
        if (is_186_or_later()) invalid_opcode();
        else set_reg8(0, flag(FLAG_CF) ? 0xFF : 0x00);
        break;
    case 0xD7: { // XLAT
        uint16_t seg = default_seg(r_.seg[DS]);
        set_reg8(0, rd8(seg, uint16_t(r_.gpr[BX] + (r_.gpr[AX] & 0xFF))));
        break;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: // ESC: no coprocessor
        decode_modrm();
        break;

    case 0xE0: case 0xE1: case 0xE2: { // LOOPNZ / LOOPZ / LOOP
        int8_t disp = int8_t(fetch8());
        r_.gpr[CX] = uint16_t(r_.gpr[CX] - 1);
        bool take = r_.gpr[CX] != 0;
        if (op == 0xE0) take = take && !flag(FLAG_ZF);
        if (op == 0xE1) take = take && flag(FLAG_ZF);
        if (take) r_.ip = uint16_t(r_.ip + disp);
        break;
    }
    case 0xE3: { // JCXZ
        int8_t disp = int8_t(fetch8());
        if (r_.gpr[CX] == 0) r_.ip = uint16_t(r_.ip + disp);
        break;
    }
    case 0xE4: set_reg8(0, in8(fetch8())); break;
    case 0xE5: r_.gpr[AX] = in16(fetch8()); break;
    case 0xE6: out8(fetch8(), uint8_t(r_.gpr[AX])); break;
    case 0xE7: out16(fetch8(), r_.gpr[AX]); break;
    case 0xE8: { // CALL near
        uint16_t disp = fetch16();
        push(r_.ip);
        r_.ip = uint16_t(r_.ip + disp);
        break;
    }
    case 0xE9: { // JMP near
        uint16_t disp = fetch16();
        r_.ip = uint16_t(r_.ip + disp);
        break;
    }
    case 0xEA: { // JMP far
        uint16_t ip = fetch16();
        uint16_t cs = fetch16();
        r_.ip = ip;
        r_.seg[CS] = cs;
        break;
    }
    case 0xEB: { // JMP short
        int8_t disp = int8_t(fetch8());
        r_.ip = uint16_t(r_.ip + disp);
        break;
    }
    case 0xEC: set_reg8(0, in8(r_.gpr[DX])); break;
    case 0xED: r_.gpr[AX] = in16(r_.gpr[DX]); break;
    case 0xEE: out8(r_.gpr[DX], uint8_t(r_.gpr[AX])); break;
    case 0xEF: out16(r_.gpr[DX], r_.gpr[AX]); break;

    case 0xF1: invalid_opcode(); break; // only reached on 186+
    case 0xF4: halted_ = true; break;
    case 0xF5: set_flag(FLAG_CF, !flag(FLAG_CF)); break;
    case 0xF6: decode_modrm(); group3(false); break;
    case 0xF7: decode_modrm(); group3(true); break;
    case 0xF8: set_flag(FLAG_CF, false); break;
    case 0xF9: set_flag(FLAG_CF, true); break;
    case 0xFA: set_flag(FLAG_IF, false); break;
    case 0xFB: set_flag(FLAG_IF, true); inhibit_irq_ = true; break;
    case 0xFC: set_flag(FLAG_DF, false); break;
    case 0xFD: set_flag(FLAG_DF, true); break;

    case 0xFE:
    case 0xFF: {
        const bool w = op == 0xFF;
        decode_modrm();
        switch (regf_) {
        case 0: rm_write(w, inc_dec(w, rm_read(w), false)); break;
        case 1: rm_write(w, inc_dec(w, rm_read(w), true)); break;
        case 2: { // CALL near indirect
            uint16_t target = rm_read(w);
            if (!w) target = uint16_t(target | 0xFF00);
            push(r_.ip);
            r_.ip = target;
            break;
        }
        case 3: { // CALL far indirect
            uint16_t ip = rd16(ea_seg_, ea_off_);
            uint16_t cs = rd16(ea_seg_, uint16_t(ea_off_ + 2));
            push(r_.seg[CS]);
            push(r_.ip);
            r_.ip = ip;
            r_.seg[CS] = cs;
            break;
        }
        case 4: { // JMP near indirect
            uint16_t target = rm_read(w);
            if (!w) target = uint16_t(target | 0xFF00);
            r_.ip = target;
            break;
        }
        case 5: { // JMP far indirect
            uint16_t ip = rd16(ea_seg_, ea_off_);
            uint16_t cs = rd16(ea_seg_, uint16_t(ea_off_ + 2));
            r_.ip = ip;
            r_.seg[CS] = cs;
            break;
        }
        case 6: case 7: { // PUSH rm (/7 is an alias)
            uint16_t v = rm_read(w);
            if (mod_ == 3 && rm_ == SP && w) {
                // PUSH SP via ModR/M behaves like opcode 54 on the 8086.
                r_.gpr[SP] = uint16_t(r_.gpr[SP] - 2);
                wr16(r_.seg[SS], r_.gpr[SP], r_.gpr[SP]);
            } else {
                if (!w) v = uint16_t(v | 0xFF00);
                push(v);
            }
            break;
        }
        }
        break;
    }

    default:
        // Every 8086 opcode is handled above; anything else is a decoder bug.
        invalid_opcode();
        break;
    }
}

// 80186 extensions.
void I86::execute_186(uint8_t op) {
    switch (op) {
    case 0x60: { // PUSHA
        uint16_t sp = r_.gpr[SP];
        push(r_.gpr[AX]); push(r_.gpr[CX]); push(r_.gpr[DX]); push(r_.gpr[BX]);
        push(sp); push(r_.gpr[BP]); push(r_.gpr[SI]); push(r_.gpr[DI]);
        break;
    }
    case 0x61: // POPA
        r_.gpr[DI] = pop(); r_.gpr[SI] = pop(); r_.gpr[BP] = pop(); pop();
        r_.gpr[BX] = pop(); r_.gpr[DX] = pop(); r_.gpr[CX] = pop(); r_.gpr[AX] = pop();
        break;
    case 0x62: { // BOUND
        decode_modrm();
        int16_t idx = int16_t(r_.gpr[regf_]);
        int16_t lo = int16_t(rd16(ea_seg_, ea_off_));
        int16_t hi = int16_t(rd16(ea_seg_, uint16_t(ea_off_ + 2)));
        if (idx < lo || idx > hi) {
            r_.ip = start_ip_;
            interrupt(5);
        }
        break;
    }
    case 0x68: push(fetch16()); break;
    case 0x6A: push(uint16_t(int16_t(int8_t(fetch8())))); break;
    case 0x69: case 0x6B: { // IMUL reg, rm, imm
        decode_modrm();
        int32_t a = int16_t(rm_read(true));
        int32_t b = op == 0x69 ? int16_t(fetch16()) : int16_t(int8_t(fetch8()));
        int32_t p = a * b;
        r_.gpr[regf_] = uint16_t(p);
        bool overflow = p != int32_t(int16_t(uint16_t(p)));
        set_flag(FLAG_CF, overflow);
        set_flag(FLAG_OF, overflow);
        step_cycles_ += 25;
        break;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        string_op(op);
        break;
    case 0xC0: case 0xC1: { // shift rm, imm8
        const bool w = op == 0xC1;
        decode_modrm();
        uint16_t v = rm_read(w);
        unsigned count = fetch8();
        uint16_t res = shift(regf_, w, v, count);
        if ((count & 0x1F) != 0) rm_write(w, res);
        break;
    }
    case 0xC8: { // ENTER
        uint16_t size = fetch16();
        uint8_t level = uint8_t(fetch8() & 0x1F);
        push(r_.gpr[BP]);
        uint16_t frame = r_.gpr[SP];
        for (unsigned i = 1; i < level; ++i) {
            r_.gpr[BP] = uint16_t(r_.gpr[BP] - 2);
            push(rd16(r_.seg[SS], r_.gpr[BP]));
        }
        if (level > 0) push(frame);
        r_.gpr[BP] = frame;
        r_.gpr[SP] = uint16_t(r_.gpr[SP] - size);
        break;
    }
    case 0xC9: // LEAVE
        r_.gpr[SP] = r_.gpr[BP];
        r_.gpr[BP] = pop();
        break;
    default:
        invalid_opcode();
        break;
    }
}

} // namespace tn34xx
