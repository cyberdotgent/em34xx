// SPDX-License-Identifier: GPL-3.0-or-later
// tn5250-probe: connect to a TN5250 server, negotiate, wait for the first
// screen and print it as text, together with the negotiation log.
//
// It never signs on and never sends user input: it waits until the host
// has written a screen and armed a read (or the keyboard unlocks), prints,
// and disconnects.
#include "host/host_config.hpp"
#include "tn5250/client.hpp"
#include "tn5250/codepage.hpp"
#include "util/strings.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace em34xx;
using namespace em34xx::tn5250;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: tn5250-probe HOST [PORT] [--ttype TYPE | --mono] [--devname NAME]\n"
                 "                    [--codepage 37|500|273] [--env NAME=VALUE] [--timeout SECONDS]\n"
                 "                    [--fields] [--tls [--insecure | --pin SHA256] [--ca FILE]\n"
                 "                    [--tls-min VER] [--tls-max VER]]\n"
                 "  --tls        TLS from the first byte; PORT defaults to 992. The server\n"
                 "               certificate is checked against the system store and HOST\n"
                 "  --insecure   accept any certificate (its SHA-256 fingerprint is printed)\n"
                 "  --pin HASH   accept only the certificate with this SHA-256 fingerprint\n"
                 "  --ca FILE    extra trusted CA certificates (PEM)\n"
                 "  --tls-min V, --tls-max V   protocol bounds: ssl3, 1.0, 1.1, 1.2 (default\n"
                 "               minimum), 1.3\n");
}

std::string row_text(const Engine& e, int r, const CodePage& cp) {
    std::string s;
    for (int c = 0; c < e.cols(); ++c) {
        const Cell& x = e.cell(r, c);
        char32_t u = U' ';
        if (!x.is_attribute() && x.ch >= 0x40 && x.ch != 0xFF && !attr_nondisplay(x.attr)) u = cp.to_unicode(x.ch);
        append_utf8(s, u);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

} // namespace

int main(int argc, char** argv) {
    Tn5250Config cfg;
    int timeout = 15;
    bool show_fields = false;
    int port = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "--ttype") cfg.terminal_type = next();
        else if (a == "--mono") cfg.terminal_type = derived_terminal_type("3477", DisplaySubtype::Green);
        else if (a == "--devname") cfg.device_name = next();
        else if (a == "--codepage") cfg.codepage = next();
        else if (a == "--timeout") timeout = std::atoi(next().c_str());
        else if (a == "--fields") show_fields = true;
        else if (a == "--tls") cfg.tls = true;
        else if (a == "--insecure") { cfg.tls = true; cfg.tls_verify = TlsVerify::None; }
        else if (a == "--pin") { cfg.tls = true; cfg.tls_cert_sha256 = next(); }
        else if (a == "--ca") { cfg.tls = true; cfg.ca_file = next(); }
        else if (a == "--tls-min" || a == "--tls-max") {
            TlsVersion& v = a == "--tls-min" ? cfg.tls_min_version : cfg.tls_max_version;
            cfg.tls = true;
            if (!parse_tls_version(next(), v)) { usage(); return 2; }
        }
        else if (a == "--env") {
            const std::string kv = next();
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) { usage(); return 2; }
            cfg.env[kv.substr(0, eq)] = kv.substr(eq + 1);
        } else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (cfg.host.empty()) cfg.host = a;
        else port = std::atoi(a.c_str());
    }
    if (cfg.host.empty()) { usage(); return 2; }
    cfg.port = uint16_t(port > 0 ? port : (cfg.tls ? 992 : 23));
    const std::string invalid = cfg.validate();
    if (!invalid.empty()) {
        std::fprintf(stderr, "tn5250-probe: %s\n", invalid.c_str());
        return 2;
    }
    const CodePage* cp = CodePage::find(cfg.codepage);
    if (!cp) {
        std::fprintf(stderr, "unsupported codepage %s (37, 500, 273)\n", cfg.codepage.c_str());
        return 2;
    }

    Client client(cfg);
    std::string err;
    if (!client.start(err)) {
        std::fprintf(stderr, "tn5250-probe: %s\n", err.c_str());
        return 1;
    }
    std::printf("== negotiation log\n");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    bool got_screen = false;
    auto settled_since = std::chrono::steady_clock::time_point{};
    while (std::chrono::steady_clock::now() < deadline) {
        client.pump();
        for (const auto& line : client.take_log()) std::printf("  %s\n", line.c_str());
        const ConnState st = client.state();
        if (st == ConnState::Failed || st == ConnState::Closed) break;
        const Engine& e = client.engine();
        const bool waiting = e.read_pending() || !e.indicators().keyboard_locked;
        if (waiting && !got_screen) {
            got_screen = true;
            settled_since = std::chrono::steady_clock::now();
        }
        // Give the host a moment for any trailing records, then stop.
        if (got_screen && std::chrono::steady_clock::now() - settled_since > std::chrono::milliseconds(500)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    client.stop();
    for (const auto& line : client.take_log()) std::printf("  %s\n", line.c_str());

    const Engine& e = client.engine();
    std::printf("== state %s%s%s\n", conn_state_name(client.state()),
                client.last_error().empty() ? "" : ": ", client.last_error().c_str());
    if (!got_screen) {
        std::printf("== no screen received within %d s\n", timeout);
        return 1;
    }
    const Indicators ind = e.indicators();
    std::printf("== screen %dx%d, CCSID %u, cursor %d,%d, keyboard %s%s\n", e.rows(), e.cols(), cp->ccsid(),
                e.cursor_row() + 1, e.cursor_col() + 1, ind.keyboard_locked ? "locked" : "unlocked",
                ind.message_waiting ? ", message waiting" : "");
    std::string rule(size_t(e.cols()), '-');
    std::printf("+%s+\n", rule.c_str());
    for (int r = 0; r < e.rows(); ++r) {
        std::string t = row_text(e, r, *cp);
        // pad to width (count code points)
        size_t cps = 0;
        for (unsigned char ch : t) if ((ch & 0xC0) != 0x80) ++cps;
        t.append(size_t(e.cols()) > cps ? size_t(e.cols()) - cps : 0, ' ');
        std::printf("|%s|\n", t.c_str());
    }
    std::printf("+%s+\n", rule.c_str());
    if (show_fields) {
        std::printf("== %zu input fields\n", e.fields().size());
        for (const Field& f : e.fields())
            std::printf("  %2d,%-3d len %3d ffw %04X attr %02X%s\n", f.start / e.cols() + 1, f.start % e.cols() + 1,
                        f.length, f.ffw, f.attr, attr_nondisplay(f.attr) ? " (non-display)" : "");
    }
    return 0;
}
