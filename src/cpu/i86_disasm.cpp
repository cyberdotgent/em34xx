#include "cpu/i86_disasm.hpp"

#include "util/strings.hpp"

namespace tn34xx {

namespace {

const char* const kReg8[] = {"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"};
const char* const kReg16[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
const char* const kSeg[] = {"es", "cs", "ss", "ds"};
const char* const kEa[] = {"bx+si", "bx+di", "bp+si", "bp+di", "si", "di", "bp", "bx"};
const char* const kAlu[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
const char* const kShift[] = {"rol", "ror", "rcl", "rcr", "shl", "shr", "setmo", "sar"};
const char* const kGrp3[] = {"test", "test", "not", "neg", "mul", "imul", "div", "idiv"};
const char* const kJcc[] = {"jo", "jno", "jb", "jnb", "jz", "jnz", "jbe", "ja",
                            "js", "jns", "jp", "jnp", "jl", "jnl", "jle", "jg"};

std::string hex(uint32_t v) { return format("0x%x", v); }

class Decoder {
public:
    Decoder(const DisasmReader& read, uint16_t cs, uint16_t ip, CpuLevel level, uint32_t mask)
        : read_(read), cs_(cs), ip_(ip), start_(ip), level_(level), mask_(mask) {}

    DisasmResult run();

private:
    uint8_t u8() { return read_(((uint32_t(cs_) << 4) + ip_++) & mask_); }
    uint16_t u16() { uint16_t lo = u8(); return uint16_t(lo | (u8() << 8)); }

    void modrm() {
        uint8_t m = u8();
        mod_ = m >> 6;
        reg_ = (m >> 3) & 7;
        rm_ = m & 7;
        if (mod_ == 3) return;
        std::string inner;
        if (mod_ == 0 && rm_ == 6) {
            inner = hex(u16());
        } else {
            inner = kEa[rm_];
            if (mod_ == 1) {
                int8_t d = int8_t(u8());
                if (d < 0) inner += "-" + hex(uint32_t(-int(d)));
                else inner += "+" + hex(uint32_t(d));
            } else if (mod_ == 2) {
                inner += "+" + hex(u16());
            }
        }
        mem_ = "[" + (seg_.empty() ? std::string() : seg_ + ":") + inner + "]";
    }

    // r/m operand with an optional size keyword for memory forms.
    std::string rm(bool w, bool size_kw = true) const {
        if (mod_ == 3) return w ? kReg16[rm_] : kReg8[rm_];
        return (size_kw ? std::string(w ? "word " : "byte ") : std::string()) + mem_;
    }
    std::string reg(bool w) const { return w ? kReg16[reg_] : kReg8[reg_]; }
    std::string rel8() { int8_t d = int8_t(u8()); return hex(uint16_t(ip_ + d)); }
    std::string rel16() { uint16_t d = u16(); return hex(uint16_t(ip_ + d)); }
    std::string moffs() { return "[" + (seg_.empty() ? std::string("") : seg_ + ":") + hex(u16()) + "]"; }

    bool is186() const { return level_ >= CpuLevel::I80186; }

    const DisasmReader& read_;
    uint16_t cs_, ip_, start_;
    CpuLevel level_;
    uint32_t mask_;
    std::string seg_, rep_;
    bool lock_ = false;
    unsigned mod_ = 0, reg_ = 0, rm_ = 0;
    std::string mem_;
    DisasmResult res_;
};

DisasmResult Decoder::run() {
    uint8_t op;
    for (int n = 0; n < 8; ++n) {
        op = u8();
        if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) seg_ = kSeg[(op >> 3) & 3];
        else if (op == 0xF2) rep_ = "repne ";
        else if (op == 0xF3) rep_ = "rep ";
        else if (op == 0xF0 || (op == 0xF1 && !is186())) lock_ = true;
        else break;
    }
    std::string t;
    const bool w = op & 1;

    if (op < 0x40 && (op & 7) < 6) {
        const char* m = kAlu[op >> 3];
        switch (op & 7) {
        case 0: case 1: modrm(); t = format("%s %s, %s", m, rm(w).c_str(), reg(w).c_str()); break;
        case 2: case 3: modrm(); t = format("%s %s, %s", m, reg(w).c_str(), rm(w).c_str()); break;
        case 4: t = format("%s al, %s", m, hex(u8()).c_str()); break;
        case 5: t = format("%s ax, %s", m, hex(u16()).c_str()); break;
        }
    } else if (is186() && op >= 0x60 && op <= 0x6F) {
        switch (op) {
        case 0x60: t = "pusha"; break;
        case 0x61: t = "popa"; break;
        case 0x62: modrm(); t = "bound " + reg(true) + ", " + rm(true, false); break;
        case 0x68: t = "push " + hex(u16()); break;
        case 0x6A: t = "push " + hex(uint16_t(int16_t(int8_t(u8())))); break;
        case 0x69: { modrm(); std::string a = rm(true); t = "imul " + reg(true) + ", " + a + ", " + hex(u16()); break; }
        case 0x6B: { modrm(); std::string a = rm(true); t = "imul " + reg(true) + ", " + a + ", " + hex(uint16_t(int16_t(int8_t(u8())))); break; }
        case 0x6C: t = rep_ + "insb"; break;
        case 0x6D: t = rep_ + "insw"; break;
        case 0x6E: t = rep_ + "outsb"; break;
        case 0x6F: t = rep_ + "outsw"; break;
        default: t = format("db 0x%02x", op); break;
        }
    } else if (op >= 0x60 && op <= 0x7F) {
        t = std::string(kJcc[op & 15]) + " " + rel8();
    } else if (op >= 0x40 && op <= 0x5F) {
        static const char* const k[] = {"inc", "dec", "push", "pop"};
        t = std::string(k[(op - 0x40) >> 3]) + " " + kReg16[op & 7];
    } else if (op >= 0x91 && op <= 0x97) {
        t = std::string("xchg ax, ") + kReg16[op & 7];
    } else if (op >= 0xB0 && op <= 0xB7) {
        t = std::string("mov ") + kReg8[op & 7] + ", " + hex(u8());
    } else if (op >= 0xB8 && op <= 0xBF) {
        t = std::string("mov ") + kReg16[op & 7] + ", " + hex(u16());
    } else if (op >= 0xD8 && op <= 0xDF) {
        modrm();
        t = format("esc 0x%02x, %s", ((op & 7) << 3) | reg_, rm(true, false).c_str());
    } else {
        switch (op) {
        case 0x06: case 0x0E: case 0x16: case 0x1E: t = std::string("push ") + kSeg[(op >> 3) & 3]; break;
        case 0x07: case 0x17: case 0x1F: t = std::string("pop ") + kSeg[(op >> 3) & 3]; break;
        case 0x0F: t = is186() ? "db 0x0f" : "pop cs"; break;
        case 0x27: t = "daa"; break;
        case 0x2F: t = "das"; break;
        case 0x37: t = "aaa"; break;
        case 0x3F: t = "aas"; break;
        case 0x80: case 0x81: case 0x82: case 0x83: {
            modrm();
            std::string dst = rm(w);
            std::string imm = op == 0x81 ? hex(u16()) : op == 0x83 ? hex(uint16_t(int16_t(int8_t(u8())))) : hex(u8());
            t = format("%s %s, %s", kAlu[reg_], dst.c_str(), imm.c_str());
            break;
        }
        case 0x84: case 0x85: modrm(); t = "test " + rm(w) + ", " + reg(w); break;
        case 0x86: case 0x87: modrm(); t = "xchg " + rm(w) + ", " + reg(w); break;
        case 0x88: case 0x89: modrm(); t = "mov " + rm(w) + ", " + reg(w); break;
        case 0x8A: case 0x8B: modrm(); t = "mov " + reg(w) + ", " + rm(w); break;
        case 0x8C: modrm(); t = "mov " + rm(true) + ", " + kSeg[reg_ & 3]; break;
        case 0x8D: modrm(); t = "lea " + reg(true) + ", " + rm(true, false); break;
        case 0x8E: modrm(); t = std::string("mov ") + kSeg[reg_ & 3] + ", " + rm(true); break;
        case 0x8F: modrm(); t = "pop " + rm(true); break;
        case 0x90: t = "nop"; break;
        case 0x98: t = "cbw"; break;
        case 0x99: t = "cwd"; break;
        case 0x9A: { uint16_t o = u16(); uint16_t s = u16(); t = format("call 0x%04x:0x%04x", s, o); res_.is_call = true; break; }
        case 0x9B: t = "wait"; break;
        case 0x9C: t = "pushf"; break;
        case 0x9D: t = "popf"; break;
        case 0x9E: t = "sahf"; break;
        case 0x9F: t = "lahf"; break;
        case 0xA0: t = "mov al, " + moffs(); break;
        case 0xA1: t = "mov ax, " + moffs(); break;
        case 0xA2: t = "mov " + moffs() + ", al"; break;
        case 0xA3: t = "mov " + moffs() + ", ax"; break;
        case 0xA4: t = rep_ + "movsb"; break;
        case 0xA5: t = rep_ + "movsw"; break;
        case 0xA6: t = (rep_ == "rep " ? std::string("repe ") : rep_) + "cmpsb"; break;
        case 0xA7: t = (rep_ == "rep " ? std::string("repe ") : rep_) + "cmpsw"; break;
        case 0xA8: t = "test al, " + hex(u8()); break;
        case 0xA9: t = "test ax, " + hex(u16()); break;
        case 0xAA: t = rep_ + "stosb"; break;
        case 0xAB: t = rep_ + "stosw"; break;
        case 0xAC: t = rep_ + "lodsb"; break;
        case 0xAD: t = rep_ + "lodsw"; break;
        case 0xAE: t = (rep_ == "rep " ? std::string("repe ") : rep_) + "scasb"; break;
        case 0xAF: t = (rep_ == "rep " ? std::string("repe ") : rep_) + "scasw"; break;
        case 0xC0: case 0xC1:
            if (is186()) { modrm(); std::string d = rm(w); t = format("%s %s, %s", kShift[reg_], d.c_str(), hex(u8()).c_str()); }
            else if (op == 0xC0) { t = "ret " + hex(u16()); res_.is_return = true; }
            else { t = "ret"; res_.is_return = true; }
            break;
        case 0xC2: t = "ret " + hex(u16()); res_.is_return = true; break;
        case 0xC3: t = "ret"; res_.is_return = true; break;
        case 0xC4: modrm(); t = "les " + reg(true) + ", " + rm(true, false); break;
        case 0xC5: modrm(); t = "lds " + reg(true) + ", " + rm(true, false); break;
        case 0xC6: case 0xC7: { modrm(); std::string d = rm(w); t = "mov " + d + ", " + (w ? hex(u16()) : hex(u8())); break; }
        case 0xC8: case 0xC9:
            if (is186() && op == 0xC8) { uint16_t sz = u16(); t = format("enter 0x%x, %u", sz, u8()); }
            else if (is186()) t = "leave";
            else if (op == 0xC8) { t = "retf " + hex(u16()); res_.is_return = true; }
            else { t = "retf"; res_.is_return = true; }
            break;
        case 0xCA: t = "retf " + hex(u16()); res_.is_return = true; break;
        case 0xCB: t = "retf"; res_.is_return = true; break;
        case 0xCC: t = "int3"; res_.is_call = true; break;
        case 0xCD: t = "int " + hex(u8()); res_.is_call = true; break;
        case 0xCE: t = "into"; res_.is_call = true; break;
        case 0xCF: t = "iret"; res_.is_return = true; break;
        case 0xD0: case 0xD1: modrm(); t = format("%s %s, 1", kShift[reg_], rm(w).c_str()); break;
        case 0xD2: case 0xD3: modrm(); t = format("%s %s, cl", kShift[reg_], rm(w).c_str()); break;
        case 0xD4: t = "aam " + hex(u8()); break;
        case 0xD5: t = "aad " + hex(u8()); break;
        case 0xD6: t = is186() ? "db 0xd6" : "salc"; break;
        case 0xD7: t = seg_.empty() ? "xlatb" : "xlat " + seg_ + ":[bx]"; break;
        case 0xE0: t = "loopnz " + rel8(); break;
        case 0xE1: t = "loopz " + rel8(); break;
        case 0xE2: t = "loop " + rel8(); break;
        case 0xE3: t = "jcxz " + rel8(); break;
        case 0xE4: t = "in al, " + hex(u8()); break;
        case 0xE5: t = "in ax, " + hex(u8()); break;
        case 0xE6: t = "out " + hex(u8()) + ", al"; break;
        case 0xE7: t = "out " + hex(u8()) + ", ax"; break;
        case 0xE8: t = "call " + rel16(); res_.is_call = true; break;
        case 0xE9: t = "jmp " + rel16(); break;
        case 0xEA: { uint16_t o = u16(); uint16_t s = u16(); t = format("jmp 0x%04x:0x%04x", s, o); break; }
        case 0xEB: t = "jmp short " + rel8(); break;
        case 0xEC: t = "in al, dx"; break;
        case 0xED: t = "in ax, dx"; break;
        case 0xEE: t = "out dx, al"; break;
        case 0xEF: t = "out dx, ax"; break;
        case 0xF1: t = "db 0xf1"; break;
        case 0xF4: t = "hlt"; break;
        case 0xF5: t = "cmc"; break;
        case 0xF6: case 0xF7: {
            modrm();
            std::string d = rm(w);
            if (reg_ < 2) t = "test " + d + ", " + (w ? hex(u16()) : hex(u8()));
            else t = std::string(kGrp3[reg_]) + " " + d;
            break;
        }
        case 0xF8: t = "clc"; break;
        case 0xF9: t = "stc"; break;
        case 0xFA: t = "cli"; break;
        case 0xFB: t = "sti"; break;
        case 0xFC: t = "cld"; break;
        case 0xFD: t = "std"; break;
        case 0xFE: case 0xFF: {
            modrm();
            switch (reg_) {
            case 0: t = "inc " + rm(w); break;
            case 1: t = "dec " + rm(w); break;
            case 2: t = "call " + rm(w); res_.is_call = true; break;
            case 3: t = "call far " + rm(w, false); res_.is_call = true; break;
            case 4: t = "jmp " + rm(w); break;
            case 5: t = "jmp far " + rm(w, false); break;
            default: t = "push " + rm(w); break;
            }
            break;
        }
        default: t = format("db 0x%02x", op); break;
        }
    }

    if (!rep_.empty() && t.compare(0, rep_.size(), rep_) != 0 &&
        t.compare(0, 5, "repe ") != 0)
        t = rep_ + t;  // REP on a non-string instruction: show it anyway
    if (lock_) t = "lock " + t;
    // Segment override on an instruction without a memory operand.
    if (!seg_.empty() && t.find(seg_ + ":") == std::string::npos) t = seg_ + " " + t;
    res_.text = t;
    res_.length = uint16_t(ip_ - start_);
    return res_;
}

} // namespace

DisasmResult disassemble(const DisasmReader& read, uint16_t cs, uint16_t ip, CpuLevel level,
                         uint32_t address_mask) {
    Decoder d(read, cs, ip, level, address_mask);
    return d.run();
}

} // namespace tn34xx
