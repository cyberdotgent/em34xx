#include "machine/machine.hpp"
#include "models/model.hpp"
#include "monitor/expr.hpp"

#include <doctest/doctest.h>

using namespace tn34xx;

namespace {
ModelInfo test_model() {
    ModelInfo m;
    m.name = "test";
    m.cpu = CpuLevel::I8088;
    m.address_bits = 20;
    return m;
}
} // namespace

TEST_CASE("expressions and addresses") {
    static const ModelInfo info = test_model();
    Machine m(info);
    m.bus().map_ram("ram", 0, 0x10000);
    m.cpu().regs().gpr[AX] = 0x1234;
    m.cpu().regs().seg[DS] = 0x0100;
    m.bus().poke8(0x1010, 0xCD);
    m.bus().poke8(0x1011, 0xAB);

    int64_t v = 0;
    std::string err;
    CHECK(eval_expr(m, "ax", v, err));
    CHECK(v == 0x1234);
    CHECK(eval_expr(m, "ah+1", v, err));
    CHECK(v == 0x13);
    CHECK(eval_expr(m, "10", v, err));
    CHECK(v == 0x10);   // hex by default
    CHECK(eval_expr(m, "#10", v, err));
    CHECK(v == 10);
    CHECK(eval_expr(m, "(1 << 4) | 3", v, err));
    CHECK(v == 0x13);
    CHECK(eval_expr(m, "ax==1234 && al!=0", v, err));
    CHECK(v == 1);
    CHECK(eval_expr(m, "word[ds:10]", v, err));
    CHECK(v == 0xABCD);
    CHECK(eval_expr(m, "byte[1011]", v, err));
    CHECK(v == 0xAB);

    uint32_t a = 0;
    CHECK(eval_address(m, "f000:fff0", a, err));
    CHECK(a == 0xFFFF0);
    CHECK(eval_address(m, "ffff:10", a, err));
    CHECK(a == 0x00000);   // wraps at 1 MiB
    CHECK(eval_address(m, "ds:ax", a, err));
    CHECK(a == 0x1000 + 0x1234);

    CHECK_FALSE(eval_expr(m, "nosuchreg + 1", v, err));
    CHECK_FALSE(eval_expr(m, "1 +", v, err));
}

TEST_CASE("register assignment") {
    static const ModelInfo info = test_model();
    Machine m(info);
    CHECK(set_register(m, "bh", 0x7F));
    CHECK(m.cpu().regs().gpr[BX] == 0x7F00);
    CHECK(set_register(m, "flags", 0));
    CHECK(m.cpu().regs().flags == 0xF002);
    CHECK_FALSE(set_register(m, "eax", 1));
}
