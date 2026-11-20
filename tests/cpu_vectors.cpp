// Runs the SingleStepTests 8088 V2 JSON test vectors against the I86 core.
//
// usage: tn34xx_cpu_vectors <testdata/8088> [opcode-file-prefix ...] [-v] [--max-fail N]
//
// Only architectural state is compared (registers, flags under the
// per-opcode undefined-flag mask, and memory). Bus-cycle traces are ignored.
#include "cpu/i86.hpp"

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <dirent.h>

using namespace tn34xx;
using nlohmann::json;

namespace {

class FlatBus : public CpuBus {
public:
    FlatBus() : mem(1u << 20, 0) {}
    uint8_t mem_read8(uint32_t a) override { return mem[a & 0xFFFFF]; }
    void mem_write8(uint32_t a, uint8_t v) override { mem[a & 0xFFFFF] = v; }
    uint8_t io_read8(uint16_t) override { return 0xFF; }
    void io_write8(uint16_t, uint8_t) override {}
    std::vector<uint8_t> mem;
};

bool read_gz(const std::string& path, std::string& out) {
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[1 << 16];
    int n;
    while ((n = gzread(f, buf, sizeof buf)) > 0) out.append(buf, size_t(n));
    gzclose(f);
    return n >= 0;
}

const char* kRegNames[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
const char* kSegNames[] = {"es", "cs", "ss", "ds"};

void load_regs(CpuRegs& r, const json& j) {
    for (int i = 0; i < 8; ++i)
        if (j.contains(kRegNames[i])) r.gpr[i] = j[kRegNames[i]].get<uint16_t>();
    for (int i = 0; i < 4; ++i)
        if (j.contains(kSegNames[i])) r.seg[i] = j[kSegNames[i]].get<uint16_t>();
    if (j.contains("ip")) r.ip = j["ip"].get<uint16_t>();
    if (j.contains("flags")) r.flags = j["flags"].get<uint16_t>();
}

std::string flag_string(uint16_t f) {
    const char* names = "odItsz?a?p?c";
    const int bits[] = {11, 10, 9, 8, 7, 6, -1, 4, -1, 2, -1, 0};
    std::string s;
    for (int i = 0; i < 12; ++i) {
        if (bits[i] < 0) continue;
        s += (f & (1u << bits[i])) ? char(toupper(names[i])) : '.';
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <testdir> [prefix...] [-v] [--max-fail N]\n", argv[0]);
        return 2;
    }
    std::string dir = argv[1];
    std::vector<std::string> prefixes;
    bool verbose = false;
    int max_fail_print = 3;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-v")) verbose = true;
        else if (!std::strcmp(argv[i], "--max-fail") && i + 1 < argc) max_fail_print = std::atoi(argv[++i]);
        else prefixes.push_back(argv[i]);
    }

    std::string meta_text;
    {
        FILE* f = std::fopen((dir + "/metadata.json").c_str(), "rb");
        if (!f) { std::fprintf(stderr, "missing %s/metadata.json\n", dir.c_str()); return 2; }
        char buf[65536]; size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) meta_text.append(buf, n);
        std::fclose(f);
    }
    json meta = json::parse(meta_text)["opcodes"];

    std::vector<std::string> files;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 8 && n.compare(n.size() - 8, 8, ".json.gz") == 0) {
                bool want = prefixes.empty();
                for (auto& p : prefixes) if (n.compare(0, p.size(), p) == 0) want = true;
                if (want) files.push_back(n);
            }
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end());

    size_t total = 0, failed_total = 0;
    int failed_files = 0;
    for (const auto& name : files) {
        std::string stem = name.substr(0, name.size() - 8);
        std::string op = stem.substr(0, 2);
        json m = meta[op];
        if (stem.size() > 2) m = m["reg"][stem.substr(3)];
        std::string status = m.value("status", "normal");
        uint16_t flags_mask = m.value("flags-mask", uint16_t(0xFFFF));
        if (status == "prefix" || status == "undefined" || status == "fpu") continue;

        std::string text;
        if (!read_gz(dir + "/" + name, text)) { std::fprintf(stderr, "read error %s\n", name.c_str()); return 2; }
        json tests = json::parse(text);

        size_t failed = 0;
        int printed = 0;
        for (const auto& t : tests) {
            FlatBus bus;
            I86 cpu(bus, CpuLevel::I8088);
            CpuRegs& r = cpu.regs();
            load_regs(r, t["initial"]["regs"]);
            for (const auto& e : t["initial"]["ram"]) bus.mem[e[0].get<uint32_t>()] = e[1].get<uint8_t>();
            CpuRegs expect = r;
            load_regs(expect, t["final"]["regs"]);

            cpu.step();

            std::string why;
            for (int i = 0; i < 8; ++i)
                if (r.gpr[i] != expect.gpr[i]) { char b[64]; std::snprintf(b, sizeof b, " %s=%04x(want %04x)", kRegNames[i], r.gpr[i], expect.gpr[i]); why += b; }
            for (int i = 0; i < 4; ++i)
                if (r.seg[i] != expect.seg[i]) { char b[64]; std::snprintf(b, sizeof b, " %s=%04x(want %04x)", kSegNames[i], r.seg[i], expect.seg[i]); why += b; }
            if (r.ip != expect.ip) { char b[64]; std::snprintf(b, sizeof b, " ip=%04x(want %04x)", r.ip, expect.ip); why += b; }
            if ((r.flags & flags_mask) != (expect.flags & flags_mask)) {
                why += " flags=" + flag_string(r.flags) + "(want " + flag_string(expect.flags) + ")";
            }
            // When an instruction raises an interrupt (e.g. a divide error) the
            // FLAGS image pushed on the stack carries the same undefined bits;
            // apply the undefined-flag mask to that stack slot too.
            const uint32_t flags_lo = cpu.linear(expect.seg[SS], uint16_t(expect.gpr[SP] + 4));
            const uint32_t flags_hi = cpu.linear(expect.seg[SS], uint16_t(expect.gpr[SP] + 5));
            const bool raised = expect.seg[CS] != t["initial"]["regs"]["cs"].get<uint16_t>() ||
                                t["final"]["regs"].contains("sp");
            for (const auto& e : t["final"]["ram"]) {
                uint32_t a = e[0].get<uint32_t>();
                uint8_t v = e[1].get<uint8_t>();
                uint8_t m = 0xFF;
                if (raised && flags_mask != 0xFFFF) {
                    if (a == flags_lo) m = uint8_t(flags_mask);
                    else if (a == flags_hi) m = uint8_t(flags_mask >> 8);
                }
                if ((bus.mem[a] & m) != (v & m)) { char b[64]; std::snprintf(b, sizeof b, " [%05x]=%02x(want %02x)", a, bus.mem[a], v); why += b; }
            }
            if (!why.empty()) {
                ++failed;
                if (printed < max_fail_print) {
                    ++printed;
                    std::printf("  FAIL %s #%s \"%s\":%s\n", stem.c_str(), t.value("idx", json(0)).dump().c_str(),
                                t["name"].get<std::string>().c_str(), why.c_str());
                }
            }
        }
        total += tests.size();
        failed_total += failed;
        if (failed) ++failed_files;
        if (failed || verbose)
            std::printf("%-8s %-12s %6zu/%zu passed\n", stem.c_str(), status.c_str(), tests.size() - failed, tests.size());
    }
    std::printf("TOTAL: %zu/%zu passed, %d file(s) with failures\n", total - failed_total, total, failed_files);
    return failed_total == 0 ? 0 : 1;
}
