// SPDX-License-Identifier: GPL-3.0-or-later
#include "gdb/gdb_stub.hpp"

#include "machine/emulator.hpp"
#include "monitor/monitor.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace em34xx {

namespace {

// i386 core register set; GDB requires every register of the feature even
// though the 8086 only has the low 16 bits of the first ten.
const char kTargetXml[] = R"(<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target version="1.0">
  <architecture>i8086</architecture>
  <feature name="org.gnu.gdb.i386.core">
    <reg name="eax" bitsize="32" type="int32" regnum="0"/>
    <reg name="ecx" bitsize="32" type="int32"/>
    <reg name="edx" bitsize="32" type="int32"/>
    <reg name="ebx" bitsize="32" type="int32"/>
    <reg name="esp" bitsize="32" type="data_ptr"/>
    <reg name="ebp" bitsize="32" type="data_ptr"/>
    <reg name="esi" bitsize="32" type="int32"/>
    <reg name="edi" bitsize="32" type="int32"/>
    <reg name="eip" bitsize="32" type="code_ptr"/>
    <reg name="eflags" bitsize="32" type="int32"/>
    <reg name="cs" bitsize="32" type="int32"/>
    <reg name="ss" bitsize="32" type="int32"/>
    <reg name="ds" bitsize="32" type="int32"/>
    <reg name="es" bitsize="32" type="int32"/>
    <reg name="fs" bitsize="32" type="int32"/>
    <reg name="gs" bitsize="32" type="int32"/>
    <reg name="st0" bitsize="80" type="i387_ext"/>
    <reg name="st1" bitsize="80" type="i387_ext"/>
    <reg name="st2" bitsize="80" type="i387_ext"/>
    <reg name="st3" bitsize="80" type="i387_ext"/>
    <reg name="st4" bitsize="80" type="i387_ext"/>
    <reg name="st5" bitsize="80" type="i387_ext"/>
    <reg name="st6" bitsize="80" type="i387_ext"/>
    <reg name="st7" bitsize="80" type="i387_ext"/>
    <reg name="fctrl" bitsize="32" type="int" group="float"/>
    <reg name="fstat" bitsize="32" type="int" group="float"/>
    <reg name="ftag" bitsize="32" type="int" group="float"/>
    <reg name="fiseg" bitsize="32" type="int" group="float"/>
    <reg name="fioff" bitsize="32" type="int" group="float"/>
    <reg name="foseg" bitsize="32" type="int" group="float"/>
    <reg name="fooff" bitsize="32" type="int" group="float"/>
    <reg name="fop" bitsize="32" type="int" group="float"/>
  </feature>
)";

// 80286 system registers, appended as registers 32.. when the core has them
// (Cpu::system_regs()). Read-only over GDB; set them from the firmware.
const char* const kSystemRegNames[] = {"msw", "gdtr_base", "gdtr_limit", "idtr_base", "idtr_limit", "ldtr", "tr"};
constexpr unsigned kNumSystemRegs = sizeof kSystemRegNames / sizeof kSystemRegNames[0];

constexpr unsigned kNumCoreRegs = 32;

std::string target_xml(bool system_regs) {
    std::string xml = kTargetXml;
    if (system_regs) {
        xml += "  <feature name=\"org.em34xx.i286.system\">\n";
        for (unsigned i = 0; i < kNumSystemRegs; ++i)
            xml += format("    <reg name=\"%s\" bitsize=\"32\" type=\"int32\" group=\"system\"%s/>\n",
                          kSystemRegNames[i], i == 0 ? format(" regnum=\"%u\"", kNumCoreRegs).c_str() : "");
        xml += "  </feature>\n";
    }
    return xml + "</target>\n";
}

// Advertised maximum packet size (data characters, excluding the frame and
// checksum; read_packet() enforces it on received packets), and the most
// monitor output bytes sent in one "O" packet (hex doubles it, so
// 2 * kMaxConsoleChunk + 1 < kPacketSize).
constexpr unsigned kPacketSize = 0x4000;
constexpr size_t kMaxConsoleChunk = 1024;

unsigned reg_size(unsigned n) { return (n >= 16 && n < 24) ? 10 : 4; }

const char kHex[] = "0123456789abcdef";

std::string to_hex(const uint8_t* p, size_t n) {
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += kHex[p[i] >> 4];
        s += kHex[p[i] & 15];
    }
    return s;
}

std::string to_hex(const std::string& text) {
    return to_hex(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool from_hex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = hexval(s[i]), lo = hexval(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(uint8_t(hi << 4 | lo));
    }
    return true;
}

std::string le32_hex(uint32_t v) {
    uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    return to_hex(b, 4);
}

uint32_t parse_hex32(const std::string& s) {
    uint64_t v = 0;
    parse_uint("0x" + s, v, 16);
    return uint32_t(v);
}

} // namespace

GdbStub::GdbStub(Emulator& emu, Monitor* monitor) : emu_(emu), monitor_(monitor) {}

GdbStub::~GdbStub() { stop(); }

bool GdbStub::start(uint16_t port, std::string& error) {
    if (!listener_.listen(port, true, error)) return false;
    stopping_.store(false);
    thread_ = std::thread([this] { thread_main(); });
    LOG_INFO("gdb", "listening on 127.0.0.1:%u (target remote :%u)", port, port);
    return true;
}

void GdbStub::stop() {
    stopping_.store(true);
    if (thread_.joinable()) thread_.join();
    listener_.close();
}

void GdbStub::wait_for_client() {
    LOG_INFO("gdb", "waiting for a debugger to attach");
    while (!connected_.load() && !stopping_.load() && !emu_.quit_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void GdbStub::thread_main() {
    while (!stopping_.load()) {
        TcpSocket client = listener_.accept(200);
        if (!client.valid()) continue;
        LOG_INFO("gdb", "debugger connected from %s", client.peer_name().c_str());
        // Attaching stops the machine.
        if (emu_.running()) {
            emu_.request_stop();
            emu_.wait_stopped();
        }
        no_ack_ = false;
        enter_no_ack_ = false;
        connected_.store(true);
        serve(client);
        connected_.store(false);
        LOG_INFO("gdb", "debugger disconnected");
    }
}

void GdbStub::serve(TcpSocket& client) {
    while (!stopping_.load()) {
        std::string packet;
        bool interrupted = false;
        if (!read_packet(client, packet, interrupted)) return;
        if (interrupted) continue;  // stray Ctrl-C while stopped
        bool detach = false;
        const std::string reply = handle(packet, client, detach);
        if (!send_packet(client, reply)) return;
        if (enter_no_ack_) {
            no_ack_ = true;
            enter_no_ack_ = false;
        }
        if (detach) return;
    }
}

bool GdbStub::read_packet(TcpSocket& client, std::string& packet, bool& interrupted) {
    packet.clear();
    interrupted = false;
    enum { Idle, Body, Sum1, Sum2 } st = Idle;
    char c;
    for (;;) {
        if (stopping_.load()) return false;
        int n = client.recv(&c, 1, 200);
        if (n == 0) continue;
        if (n < 0) return false;
        switch (st) {
        case Idle:
            if (c == '$') st = Body;
            else if (c == '\x03') { interrupted = true; return true; }
            break;
        case Body:
            if (c == '#') st = Sum1;
            else if (packet.size() >= kPacketSize) {
                // No client honouring PacketSize sends this; drop the link
                // rather than buffer without bound.
                LOG_WARN("gdb", "packet exceeds PacketSize (%u bytes); dropping the connection", kPacketSize);
                return false;
            } else packet += c;
            break;
        case Sum1:
            st = Sum2;
            break;
        case Sum2:
            if (!no_ack_) client.send_all("+", 1);
            return true;
        }
    }
}

bool GdbStub::send_packet(TcpSocket& client, const std::string& payload) {
    uint8_t sum = 0;
    for (char ch : payload) sum = uint8_t(sum + uint8_t(ch));
    std::string frame = "$" + payload + "#" + kHex[sum >> 4] + kHex[sum & 15];
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!client.send_all(frame.data(), frame.size())) return false;
        if (no_ack_) return true;
        char c;
        for (;;) {
            int n = client.recv(&c, 1, 2000);
            if (n <= 0) return false;
            if (c == '+') return true;
            if (c == '-') break;
        }
    }
    return false;
}

std::string GdbStub::stop_reply() {
    const StopEvent ev = emu_.last_stop();
    if (ev.reason == StopEvent::Reason::Watchpoint) {
        const char* kind = "watch";
        emu_.with_machine([&](Machine& m) {
            for (const Watchpoint& w : m.bus().watchpoints()) {
                if (w.id != ev.id) continue;
                if (w.kind == AccessKind::Read) kind = "rwatch";
                else if (w.kind == AccessKind::ReadWrite) kind = "awatch";
            }
        });
        return format("T05%s:%x;", kind, ev.data_address);
    }
    if (ev.reason == StopEvent::Reason::BusFault) return "S0a";  // SIGBUS
    if (ev.reason == StopEvent::Reason::User) return "S02";      // SIGINT
    return format("S%02x", last_signal_);
}

std::string GdbStub::resume_and_report(TcpSocket& client, bool single_step) {
    if (single_step) {
        emu_.step(1);
        return stop_reply();
    }
    emu_.resume();
    for (;;) {
        StopEvent ev;
        if (emu_.wait_stopped_for(20, ev)) break;
        char c;
        int n = client.recv(&c, 1, 0);
        if (n < 0) {
            // Debugger went away while running; leave the machine running.
            return {};
        }
        if (n > 0 && c == '\x03') emu_.request_stop();
        if (stopping_.load()) emu_.request_stop();
    }
    const StopEvent ev = emu_.last_stop();
    if (ev.reason == StopEvent::Reason::BusFault)
        LOG_WARN("gdb", "stopped by %s", ev.detail.c_str());
    return stop_reply();
}

std::string GdbStub::read_register(unsigned n) {
    return emu_.with_machine([&](Machine& m) -> std::string {
        const CpuRegs& r = m.cpu().regs();
        static const int gpr_map[8] = {AX, CX, DX, BX, SP, BP, SI, DI};
        const SystemRegs* s = m.cpu().system_regs();
        if (n >= kNumCoreRegs && s) {
            switch (n - kNumCoreRegs) {
            case 0: return le32_hex(s->msw);
            case 1: return le32_hex(s->gdtr.base);
            case 2: return le32_hex(s->gdtr.limit);
            case 3: return le32_hex(s->idtr.base);
            case 4: return le32_hex(s->idtr.limit);
            case 5: return le32_hex(s->ldtr.selector);
            case 6: return le32_hex(s->tr.selector);
            default: return std::string();
            }
        }
        if (n < 8) return le32_hex(r.gpr[gpr_map[n]]);
        if (n == 8) return le32_hex(m.cpu().pc_linear());
        if (n == 9) return le32_hex(r.flags);
        if (n == 10) return le32_hex(r.seg[CS]);
        if (n == 11) return le32_hex(r.seg[SS]);
        if (n == 12) return le32_hex(r.seg[DS]);
        if (n == 13) return le32_hex(r.seg[ES]);
        if (n == 14 || n == 15) return le32_hex(0);
        return std::string(reg_size(n) * 2, '0');
    });
}

bool GdbStub::write_register(unsigned n, uint32_t v) {
    return emu_.with_machine([&](Machine& m) {
        CpuRegs& r = m.cpu().regs();
        static const int gpr_map[8] = {AX, CX, DX, BX, SP, BP, SI, DI};
        if (n < 8) { r.gpr[gpr_map[n]] = uint16_t(v); return true; }
        if (n == 8) {
            // eip is linear; translate into the current code segment
            // (segment or, in protected mode, selector base).
            uint32_t base = 0;
            if (!m.cpu().translate(r.seg[CS], 0, base)) return false;
            if (v < base || v - base > 0xFFFF) return false;
            r.ip = uint16_t(v - base);
            return true;
        }
        if (n == 9) { r.flags = m.cpu().normalise_flags(uint16_t(v)); return true; }
        if (n == 10) { r.seg[CS] = uint16_t(v); return true; }
        if (n == 11) { r.seg[SS] = uint16_t(v); return true; }
        if (n == 12) { r.seg[DS] = uint16_t(v); return true; }
        if (n == 13) { r.seg[ES] = uint16_t(v); return true; }
        return n < kNumCoreRegs;  // fs/gs/x87: accepted and ignored; system registers: read-only
    });
}

std::string GdbStub::read_registers() {
    std::string s;
    const bool sys = emu_.with_machine([](Machine& m) { return m.cpu().system_regs() != nullptr; });
    const unsigned count = kNumCoreRegs + (sys ? kNumSystemRegs : 0);
    for (unsigned n = 0; n < count; ++n) s += read_register(n);
    return s;
}

bool GdbStub::write_registers(const std::string& hex) {
    std::vector<uint8_t> bytes;
    if (!from_hex(hex, bytes)) return false;
    size_t off = 0;
    for (unsigned n = 0; n < 16 && off + 4 <= bytes.size(); ++n, off += 4) {
        uint32_t v = bytes[off] | (bytes[off + 1] << 8) | (bytes[off + 2] << 16) | (uint32_t(bytes[off + 3]) << 24);
        if (n == 8) {
            // Skip eip when unchanged so a G packet does not fail on a
            // linear address outside the current segment window.
            uint32_t cur = emu_.with_machine([](Machine& m) { return m.cpu().pc_linear(); });
            if (v == cur) continue;
        }
        write_register(n, v);
    }
    return true;
}

std::string GdbStub::read_memory(uint32_t addr, uint32_t len) {
    std::vector<uint8_t> buf(len);
    emu_.with_machine([&](Machine& m) {
        for (uint32_t i = 0; i < len; ++i) buf[i] = m.bus().peek8(addr + i);
    });
    return to_hex(buf.data(), buf.size());
}

bool GdbStub::write_memory(uint32_t addr, const std::string& hex) {
    std::vector<uint8_t> bytes;
    if (!from_hex(hex, bytes)) return false;
    return emu_.with_machine([&](Machine& m) {
        for (size_t i = 0; i < bytes.size(); ++i)
            if (!m.bus().poke8(addr + uint32_t(i), bytes[i])) return false;
        return true;
    });
}

std::string GdbStub::query(const std::string& p) {
    if (starts_with(p, "qSupported")) return format("PacketSize=%x;", kPacketSize) + "qXfer:features:read+;QStartNoAckMode+";
    if (p == "qAttached") return "1";
    if (p == "qC") return "QC1";
    if (p == "qfThreadInfo") return "m1";
    if (p == "qsThreadInfo") return "l";
    if (p == "qSymbol::") return "OK";
    if (starts_with(p, "qXfer:features:read:target.xml:")) {
        const std::string range = p.substr(std::strlen("qXfer:features:read:target.xml:"));
        const auto comma = range.find(',');
        const uint32_t off = parse_hex32(range.substr(0, comma));
        const uint32_t len = parse_hex32(range.substr(comma + 1));
        const bool sys = emu_.with_machine([](Machine& m) { return m.cpu().system_regs() != nullptr; });
        const std::string xml = target_xml(sys);
        if (off >= xml.size()) return "l";
        const std::string chunk = xml.substr(off, len);
        return (off + chunk.size() >= xml.size() ? "l" : "m") + chunk;
    }
    return {};
}

std::string GdbStub::monitor_command(const std::string& hex_cmd, TcpSocket& client) {
    std::vector<uint8_t> bytes;
    if (!from_hex(hex_cmd, bytes)) return "E01";
    const std::string cmd(bytes.begin(), bytes.end());
    if (!monitor_) return to_hex(std::string("no monitor available\n"));

    // Stream the output as "O" packets of at most kMaxConsoleChunk bytes,
    // flushing whenever a full chunk is pending; GDB prints each as it
    // arrives. A command error is reported in the output text ("error:
    // ...") and the reply is still OK: an "E" reply only makes GDB print
    // "Protocol error with Rcmd".
    std::string pending;
    bool link_ok = true;
    auto flush = [&](bool all) {
        while (link_ok && (pending.size() >= kMaxConsoleChunk || (all && !pending.empty()))) {
            const size_t n = std::min(pending.size(), kMaxConsoleChunk);
            link_ok = send_packet(client, "O" + to_hex(pending.substr(0, n)));
            pending.erase(0, n);
        }
    };
    monitor_->execute_streamed(cmd, [&](const std::string& text) {
        pending += text;
        flush(false);
    });
    flush(true);
    return "OK";
}

std::string GdbStub::handle(const std::string& p, TcpSocket& client, bool& detach) {
    if (p.empty()) return {};
    switch (p[0]) {
    case '?':
        return stop_reply();
    case 'g':
        return read_registers();
    case 'G':
        return write_registers(p.substr(1)) ? "OK" : "E01";
    case 'p':
        return read_register(parse_hex32(p.substr(1)));
    case 'P': {
        const auto eq = p.find('=');
        std::vector<uint8_t> b;
        if (eq == std::string::npos || !from_hex(p.substr(eq + 1), b) || b.empty()) return "E01";
        uint32_t v = 0;
        for (size_t i = 0; i < b.size() && i < 4; ++i) v |= uint32_t(b[i]) << (8 * i);
        return write_register(parse_hex32(p.substr(1, eq - 1)), v) ? "OK" : "E01";
    }
    case 'm': {
        const auto comma = p.find(',');
        if (comma == std::string::npos) return "E01";
        uint32_t len = parse_hex32(p.substr(comma + 1));
        if (len > 0x1000) len = 0x1000;
        return read_memory(parse_hex32(p.substr(1, comma - 1)), len);
    }
    case 'M': {
        const auto comma = p.find(',');
        const auto colon = p.find(':');
        if (comma == std::string::npos || colon == std::string::npos) return "E01";
        return write_memory(parse_hex32(p.substr(1, comma - 1)), p.substr(colon + 1)) ? "OK" : "E0e";
    }
    case 'c':
    case 's': {
        if (p.size() > 1) {
            // Resume address is linear; move IP within CS.
            write_register(8, parse_hex32(p.substr(1)));
        }
        return resume_and_report(client, p[0] == 's');
    }
    case 'C':
    case 'S': {
        // C/S<sig>[;addr]: GDB sends these to continue/step after a stop it
        // passes a signal for (e.g. SIGBUS after a bus fault). There is
        // nothing to deliver a signal to, so it is dropped.
        const auto semi = p.find(';');
        if (semi != std::string::npos) write_register(8, parse_hex32(p.substr(semi + 1)));
        return resume_and_report(client, p[0] == 'S');
    }
    case 'Z':
    case 'z': {
        // Z<type>,<addr>,<kind>
        const bool insert = p[0] == 'Z';
        const auto parts = split(p.substr(1), ',');
        if (parts.size() < 3) return "E01";
        const unsigned type = unsigned(parse_hex32(parts[0]));
        const uint32_t addr = parse_hex32(parts[1]);
        const uint32_t len = parse_hex32(parts[2]);
        if (type <= 1) {
            emu_.with_machine([&](Machine& m) {
                if (insert) m.add_breakpoint(addr);
                else m.remove_breakpoint_at(addr);
            });
            return "OK";
        }
        if (type <= 4) {
            const AccessKind kind = type == 2 ? AccessKind::Write : type == 3 ? AccessKind::Read : AccessKind::ReadWrite;
            const bool ok = emu_.with_machine([&](Machine& m) {
                if (insert) { m.bus().add_watchpoint(addr, len, kind); return true; }
                return m.bus().remove_watchpoint_at(addr, len, kind);
            });
            return ok ? "OK" : "E01";
        }
        return {};
    }
    case 'k':
        detach = true;
        return "OK";
    case 'D':
        detach = true;
        emu_.resume();
        return "OK";
    case 'H':
        return "OK";
    case 'T':
        return "OK";
    case 'q':
        if (starts_with(p, "qRcmd,")) return monitor_command(p.substr(6), client);
        return query(p);
    case 'Q':
        if (p == "QStartNoAckMode") {
            // The OK reply itself is still acknowledged; serve() switches
            // to no-ack mode after sending it.
            enter_no_ack_ = true;
            return "OK";
        }
        return {};
    case 'v':
        if (p == "vMustReplyEmpty") return {};
        if (starts_with(p, "vKill")) { detach = true; return "OK"; }
        return {};
    default:
        return {};
    }
}

} // namespace em34xx
