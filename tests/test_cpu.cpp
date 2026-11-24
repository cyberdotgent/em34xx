// SPDX-License-Identifier: GPL-3.0-or-later
// Small end-to-end programs; exhaustive per-instruction coverage comes from
// the SingleStepTests vectors (cpu_vectors).
#include "cpu/i86.hpp"
#include "cpu/i86_disasm.hpp"

#include <doctest/doctest.h>
#include <initializer_list>
#include <string>

#include <algorithm>
#include <vector>

using namespace em34xx;

namespace {

struct RamBus : CpuBus {
    std::vector<uint8_t> mem = std::vector<uint8_t>(1u << 20, 0);
    std::vector<std::pair<uint16_t, uint8_t>> out;
    uint8_t vector = 0x20;
    uint8_t mem_read8(uint32_t a) override { return mem[a & 0xFFFFF]; }
    void mem_write8(uint32_t a, uint8_t v) override { mem[a & 0xFFFFF] = v; }
    uint8_t io_read8(uint16_t p) override { return uint8_t(p); }
    void io_write8(uint16_t p, uint8_t v) override { out.emplace_back(p, v); }
    uint8_t interrupt_ack() override { return vector; }
    void load(uint32_t at, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) mem[at++] = b;
    }
};

} // namespace

TEST_CASE("reset vector") {
    RamBus bus;
    I86 cpu(bus, CpuLevel::I8088);
    CHECK(cpu.regs().seg[CS] == 0xFFFF);
    CHECK(cpu.regs().ip == 0);
    CHECK(cpu.pc_linear() == 0xFFFF0);
    CHECK(cpu.regs().flags == 0xF002);
}

TEST_CASE("Cpu interface: real-mode translation and no system registers") {
    RamBus bus;
    std::unique_ptr<Cpu> cpu = make_cpu(bus, CpuLevel::I80188);
    CHECK(cpu->level() == CpuLevel::I80188);
    uint32_t lin = 0;
    REQUIRE(cpu->translate(0x1234, 0x0010, lin));
    CHECK(lin == 0x12350);
    REQUIRE(cpu->translate(0xFFFF, 0x0010, lin));
    CHECK(lin == 0x00000);   // 20-bit wrap
    CHECK_FALSE(cpu->translate(0x0000, 0x10000, lin));
    CHECK(cpu->debug_linear(0xF000, 0xFFF0) == 0xFFFF0);
    CHECK_FALSE(cpu->protected_mode());
    CHECK(cpu->system_regs() == nullptr);
}

TEST_CASE("far jump, loop and string store") {
    RamBus bus;
    // FFFF:0000  jmp f000:0100
    bus.load(0xFFFF0, {0xEA, 0x00, 0x01, 0x00, 0xF0});
    // F000:0100  xor ax,ax; mov es,ax; mov di,0x200; mov cx,4; mov al,0x5a; rep stosb; hlt
    bus.load(0xF0100, {0x31, 0xC0, 0x8E, 0xC0, 0xBF, 0x00, 0x02, 0xB9, 0x04, 0x00, 0xB0, 0x5A, 0xF3, 0xAA, 0xF4});
    I86 cpu(bus, CpuLevel::I8088);
    for (int i = 0; i < 20 && !cpu.halted(); ++i) cpu.step();
    REQUIRE(cpu.halted());
    for (uint32_t a = 0x200; a < 0x204; ++a) CHECK(bus.mem[a] == 0x5A);
    CHECK(bus.mem[0x204] == 0);
    CHECK(cpu.regs().gpr[CX] == 0);
    CHECK(cpu.regs().gpr[DI] == 0x204);
}

TEST_CASE("hardware interrupt wakes HLT and IRET returns") {
    RamBus bus;
    // IVT entry 0x20 -> 0000:0400
    bus.load(0x80, {0x00, 0x04, 0x00, 0x00});
    // 0000:0400  mov al,0x11; out 0x40,al; iret
    bus.load(0x400, {0xB0, 0x11, 0xE6, 0x40, 0xCF});
    // 0000:0500  sti; hlt; mov bl,0x22
    bus.load(0x500, {0xFB, 0xF4, 0xB3, 0x22});
    I86 cpu(bus, CpuLevel::I8088);
    cpu.regs().seg[CS] = 0;
    cpu.regs().ip = 0x500;
    cpu.regs().seg[SS] = 0;
    cpu.regs().gpr[SP] = 0x1000;
    cpu.step();  // sti
    cpu.step();  // hlt
    CHECK(cpu.halted());
    cpu.step();
    CHECK(cpu.halted());
    cpu.set_intr(true);
    cpu.step();  // interrupt taken, first handler instruction executed
    cpu.set_intr(false);
    CHECK_FALSE(cpu.halted());
    cpu.step();  // out
    cpu.step();  // iret
    REQUIRE(bus.out.size() == 1);
    CHECK(bus.out[0].first == 0x40);
    CHECK(bus.out[0].second == 0x11);
    cpu.step();  // mov bl,0x22
    CHECK((cpu.regs().gpr[BX] & 0xFF) == 0x22);
    CHECK(cpu.regs().gpr[SP] == 0x1000);
}

TEST_CASE("a segment full of prefix bytes does not hang step()") {
    for (uint8_t prefix : {uint8_t(0x26), uint8_t(0xF0), uint8_t(0xF3)}) {
        RamBus bus;
        std::fill(bus.mem.begin(), bus.mem.end(), prefix);
        I86 cpu(bus, CpuLevel::I8088);
        uint64_t cycles = cpu.cycles();
        for (int i = 0; i < 200; ++i) {   // more than 64 KB of prefixes in all
            CHECK(cpu.step() > 0);
            CHECK(cpu.cycles() > cycles);
            cycles = cpu.cycles();
        }
        CHECK(cpu.regs().seg[CS] == 0xFFFF);
        // The chain still ends in whatever instruction follows it.
        bus.mem[cpu.pc_linear() + 3] = 0xF4;   // hlt
        cpu.step();
        CHECK(cpu.halted());
    }
}

TEST_CASE("80186 extensions are level dependent") {
    RamBus bus;
    // push 0x1234 (68 34 12)
    bus.load(0x100, {0x68, 0x34, 0x12});
    SUBCASE("80188 executes PUSH imm16") {
        I86 cpu(bus, CpuLevel::I80188);
        cpu.regs().seg[CS] = 0;
        cpu.regs().ip = 0x100;
        cpu.regs().gpr[SP] = 0x200;
        cpu.step();
        CHECK(cpu.regs().gpr[SP] == 0x1FE);
        CHECK(bus.mem[0x1FE] == 0x34);
        CHECK(bus.mem[0x1FF] == 0x12);
    }
    SUBCASE("8088 treats 68 as an alias of JS") {
        I86 cpu(bus, CpuLevel::I8088);
        cpu.regs().seg[CS] = 0;
        cpu.regs().ip = 0x100;
        cpu.regs().gpr[SP] = 0x200;
        cpu.step();
        CHECK(cpu.regs().gpr[SP] == 0x200);
        CHECK(cpu.regs().ip == 0x102);  // SF clear: not taken
    }
}

TEST_CASE("NEC V20: the 80186 instructions, its own edge cases and faster timing") {
    RamBus bus;
    auto at = [&](I86& cpu, std::initializer_list<uint8_t> code) {
        bus.load(0x100, code);
        cpu.regs().seg[CS] = 0;
        cpu.regs().seg[DS] = 0;
        cpu.regs().seg[SS] = 0;
        cpu.regs().ip = 0x100;
        cpu.regs().gpr[SP] = 0x200;
    };
    I86 v20(bus, CpuLevel::NecV20), i88(bus, CpuLevel::I8088), i188(bus, CpuLevel::I80188);
    CHECK(std::string(cpu_level_name(CpuLevel::NecV20)) == "V20");
    CHECK(v20.is_186_or_later());
    CHECK((v20.normalise_flags(0) & 0xF000) == 0xF000);

    // PUSH imm16 (an 80186 instruction).
    at(v20, {0x68, 0x34, 0x12});
    v20.step();
    CHECK(v20.regs().gpr[SP] == 0x1FE);
    CHECK(bus.mem[0x1FE] == 0x34);

    // PUSH SP pushes the decremented value, as on the 8088.
    at(v20, {0x54});
    v20.step();
    CHECK(bus.mem[0x1FE] == 0xFE);
    CHECK(bus.mem[0x1FF] == 0x01);

    // Shift counts are not masked: SHL AX by 33 clears it (the 80188 shifts by 1).
    for (I86* cpu : {&v20, &i188}) {
        at(*cpu, {0xC1, 0xE0, 0x21});   // shl ax, 33
        cpu->regs().gpr[AX] = 0x0003;
        cpu->step();
    }
    CHECK(v20.regs().gpr[AX] == 0x0000);
    CHECK(i188.regs().gpr[AX] == 0x0006);

    // AAD ignores its operand (always base 10).
    for (I86* cpu : {&v20, &i88}) {
        at(*cpu, {0xD5, 0x10});   // aad 16
        cpu->regs().gpr[AX] = 0x0203;
        cpu->step();
    }
    CHECK(v20.regs().gpr[AX] == 23);
    CHECK(i88.regs().gpr[AX] == 0x23);

    // D6 is an alias of XLAT, not SALC.
    at(v20, {0xD6});
    bus.mem[0x305] = 0x5A;
    v20.regs().gpr[BX] = 0x300;
    v20.regs().gpr[AX] = 0x0005;
    v20.step();
    CHECK((v20.regs().gpr[AX] & 0xFF) == 0x5A);

    // An undefined opcode does not trap: the 80188 takes vector 6, the V20 goes on.
    at(v20, {0xF1, 0x90});
    v20.step();
    CHECK(v20.regs().seg[CS] == 0);
    CHECK(v20.regs().gpr[SP] == 0x200);

    // A memory operand and a multiply cost fewer clocks than on the 8088.
    uint32_t clocks[2];
    int k = 0;
    for (I86* cpu : {&v20, &i88}) {
        at(*cpu, {0xF7, 0xA7, 0x00, 0x03});   // mul word [bx+0300]
        cpu->regs().gpr[BX] = 0;
        cpu->regs().gpr[AX] = 7;
        clocks[k++] = cpu->step();
    }
    CHECK(clocks[0] < clocks[1]);
    CHECK(clocks[1] - clocks[0] == (118 - 30) + (5 - 2));
}

TEST_CASE("disassembler") {
    RamBus bus;
    auto rd = [&](uint32_t a) { return bus.mem[a]; };
    bus.load(0x0, {0xEA, 0x00, 0x00, 0x00, 0xF0});
    CHECK(disassemble(rd, 0, 0, CpuLevel::I8088).text == "jmp 0xf000:0x0000");
    bus.load(0x10, {0x26, 0x89, 0x47, 0x10});
    auto d = disassemble(rd, 0, 0x10, CpuLevel::I8088);
    CHECK(d.text == "mov word [es:bx+0x10], ax");
    CHECK(d.length == 4);
    bus.load(0x20, {0xF3, 0xA4});
    CHECK(disassemble(rd, 0, 0x20, CpuLevel::I8088).text == "rep movsb");
    bus.load(0x30, {0xE8, 0xFD, 0xFF});
    d = disassemble(rd, 0, 0x30, CpuLevel::I8088);
    CHECK(d.text == "call 0x30");
    CHECK(d.is_call);
}
