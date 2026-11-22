// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor/expr.hpp"

#include "machine/machine.hpp"
#include "util/strings.hpp"

#include <cctype>
#include <cstring>

namespace em34xx {

namespace {

const char* const kR16[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
const char* const kR8[] = {"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"};
const char* const kSeg[] = {"es", "cs", "ss", "ds"};

class Parser {
public:
    Parser(Machine& m, const std::string& s) : m_(m), s_(s) {}

    bool parse_expr(int64_t& out) { return logic_or(out); }

    bool parse_address(uint32_t& out) {
        int64_t a;
        if (!logic_or(a)) return false;
        skip();
        if (peek() == ':') {
            ++pos_;
            int64_t off;
            if (!logic_or(off)) return false;
            out = m_.cpu().debug_linear(uint16_t(a), uint16_t(off));
        } else {
            out = uint32_t(a) & m_.bus().address_mask();
        }
        return true;
    }

    bool at_end() { skip(); return pos_ >= s_.size(); }
    std::string error;

private:
    void skip() { while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_; }
    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    bool eat(const char* op) {
        skip();
        const size_t n = std::strlen(op);
        if (s_.compare(pos_, n, op) == 0) {
            // Do not split "<<" into "<" "<", "&&" into "&" "&", etc.
            if (n == 1 && pos_ + 1 < s_.size()) {
                const char next = s_[pos_ + 1];
                if ((op[0] == '<' || op[0] == '>') && (next == op[0] || next == '=')) return false;
                if ((op[0] == '&' || op[0] == '|') && next == op[0]) return false;
                if ((op[0] == '=' || op[0] == '!') && next == '=') return false;
            }
            pos_ += n;
            return true;
        }
        return false;
    }
    bool fail(const std::string& e) { if (error.empty()) error = e; return false; }

    bool logic_or(int64_t& v) {
        if (!logic_and(v)) return false;
        while (eat("||")) { int64_t r; if (!logic_and(r)) return false; v = (v || r) ? 1 : 0; }
        return true;
    }
    bool logic_and(int64_t& v) {
        if (!compare(v)) return false;
        while (eat("&&")) { int64_t r; if (!compare(r)) return false; v = (v && r) ? 1 : 0; }
        return true;
    }
    bool compare(int64_t& v) {
        if (!bit_or(v)) return false;
        for (;;) {
            int64_t r;
            if (eat("==")) { if (!bit_or(r)) return false; v = v == r; }
            else if (eat("!=")) { if (!bit_or(r)) return false; v = v != r; }
            else if (eat("<=")) { if (!bit_or(r)) return false; v = v <= r; }
            else if (eat(">=")) { if (!bit_or(r)) return false; v = v >= r; }
            else if (eat("<")) { if (!bit_or(r)) return false; v = v < r; }
            else if (eat(">")) { if (!bit_or(r)) return false; v = v > r; }
            else return true;
        }
    }
    bool bit_or(int64_t& v) {
        if (!bit_xor(v)) return false;
        while (eat("|")) { int64_t r; if (!bit_xor(r)) return false; v |= r; }
        return true;
    }
    bool bit_xor(int64_t& v) {
        if (!bit_and(v)) return false;
        while (eat("^")) { int64_t r; if (!bit_and(r)) return false; v ^= r; }
        return true;
    }
    bool bit_and(int64_t& v) {
        if (!shift(v)) return false;
        while (eat("&")) { int64_t r; if (!shift(r)) return false; v &= r; }
        return true;
    }
    bool shift(int64_t& v) {
        if (!add(v)) return false;
        for (;;) {
            int64_t r;
            if (eat("<<")) { if (!add(r)) return false; v <<= (r & 63); }
            else if (eat(">>")) { if (!add(r)) return false; v >>= (r & 63); }
            else return true;
        }
    }
    bool add(int64_t& v) {
        if (!mul(v)) return false;
        for (;;) {
            int64_t r;
            if (eat("+")) { if (!mul(r)) return false; v += r; }
            else if (eat("-")) { if (!mul(r)) return false; v -= r; }
            else return true;
        }
    }
    bool mul(int64_t& v) {
        if (!unary(v)) return false;
        for (;;) {
            int64_t r;
            if (eat("*")) { if (!unary(r)) return false; v *= r; }
            else if (eat("/")) { if (!unary(r)) return false; if (!r) return fail("division by zero"); v /= r; }
            else if (eat("%")) { if (!unary(r)) return false; if (!r) return fail("division by zero"); v %= r; }
            else return true;
        }
    }
    // Every recursive cycle in the grammar ('(' ... ')', byte[...]/word[...],
    // chained unary operators) passes through unary(), so this is the one
    // place that bounds host stack use.
    bool unary(int64_t& v) {
        const DepthGuard guard(depth_);
        if (depth_ > kMaxDepth) return fail("expression nested too deeply");
        if (eat("-")) { if (!unary(v)) return false; v = -v; return true; }
        if (eat("~")) { if (!unary(v)) return false; v = ~v; return true; }
        if (eat("!")) { if (!unary(v)) return false; v = !v; return true; }
        return primary(v);
    }
    bool primary(int64_t& v) {
        skip();
        if (eat("(")) {
            if (!logic_or(v)) return false;
            if (!eat(")")) return fail("missing ')'");
            return true;
        }
        const size_t start = pos_;
        while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_' ||
                                    s_[pos_] == '#'))
            ++pos_;
        const std::string word = s_.substr(start, pos_ - start);
        if (word.empty()) return fail(format("unexpected '%c'", peek()));
        const std::string lw = to_lower(word);

        if (lw == "byte" || lw == "word") {
            if (!eat("[")) return fail("expected '[' after " + lw);
            uint32_t addr;
            if (!parse_address(addr)) return false;
            if (!eat("]")) return fail("missing ']'");
            const uint8_t lo = m_.bus().peek8(addr);
            if (lw == "byte") { v = lo; return true; }
            v = lo | (m_.bus().peek8(addr + 1) << 8);
            return true;
        }
        uint16_t reg;
        if (get_register(m_, lw, reg)) {
            v = reg;
            return true;
        }
        if (lw == "pc") { v = m_.cpu().pc_linear(); return true; }
        uint64_t n;
        if (parse_uint(word, n, 16)) { v = int64_t(n); return true; }
        return fail("unknown symbol '" + word + "'");
    }

    // Far beyond anything typed by hand, and small enough for a thread with
    // a small stack: a parenthesis level costs about eleven frames.
    static constexpr int kMaxDepth = 128;

    struct DepthGuard {
        explicit DepthGuard(int& d) : d_(d) { ++d_; }
        ~DepthGuard() { --d_; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
        int& d_;
    };

    Machine& m_;
    const std::string& s_;
    size_t pos_ = 0;
    int depth_ = 0;
};

} // namespace

bool get_register(Machine& m, const std::string& name, uint16_t& value) {
    const std::string n = to_lower(name);
    const CpuRegs& r = m.cpu().regs();
    for (int i = 0; i < 8; ++i) {
        if (n == kR16[i]) { value = r.gpr[i]; return true; }
        if (n == kR8[i]) { value = i < 4 ? uint8_t(r.gpr[i]) : uint8_t(r.gpr[i - 4] >> 8); return true; }
    }
    for (int i = 0; i < 4; ++i)
        if (n == kSeg[i]) { value = r.seg[i]; return true; }
    if (n == "ip") { value = r.ip; return true; }
    if (n == "flags" || n == "fl") { value = r.flags; return true; }
    return false;
}

bool set_register(Machine& m, const std::string& name, uint16_t value) {
    const std::string n = to_lower(name);
    CpuRegs& r = m.cpu().regs();
    for (int i = 0; i < 8; ++i) {
        if (n == kR16[i]) { r.gpr[i] = value; return true; }
        if (n == kR8[i]) {
            if (i < 4) r.gpr[i] = uint16_t((r.gpr[i] & 0xFF00) | (value & 0xFF));
            else r.gpr[i - 4] = uint16_t((r.gpr[i - 4] & 0x00FF) | ((value & 0xFF) << 8));
            return true;
        }
    }
    for (int i = 0; i < 4; ++i)
        if (n == kSeg[i]) { r.seg[i] = value; return true; }
    if (n == "ip") { r.ip = value; return true; }
    if (n == "flags" || n == "fl") { r.flags = m.cpu().normalise_flags(value); return true; }
    return false;
}

bool eval_expr(Machine& m, const std::string& text, int64_t& out, std::string& error) {
    Parser p(m, text);
    if (!p.parse_expr(out)) { error = p.error; return false; }
    if (!p.at_end()) { error = "unexpected trailing text in '" + text + "'"; return false; }
    return true;
}

bool eval_address(Machine& m, const std::string& text, uint32_t& linear, std::string& error) {
    Parser p(m, text);
    if (!p.parse_address(linear)) { error = p.error; return false; }
    if (!p.at_end()) { error = "unexpected trailing text in '" + text + "'"; return false; }
    return true;
}

} // namespace em34xx
