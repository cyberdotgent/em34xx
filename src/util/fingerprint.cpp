// SPDX-License-Identifier: GPL-3.0-or-later
#include "util/fingerprint.hpp"

#include "util/strings.hpp"

namespace tn34xx {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string format_sha256_fingerprint(const Sha256Digest& digest) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(digest.size() * 3);
    for (size_t i = 0; i < digest.size(); ++i) {
        if (i) out += ':';
        out += kHex[digest[i] >> 4];
        out += kHex[digest[i] & 0x0F];
    }
    return out;
}

bool parse_sha256_fingerprint(const std::string& text, Sha256Digest& out) {
    std::string t = trim(text);
    const std::string lower = to_lower(t);
    for (const char* prefix : {"sha256:", "sha256=", "sha-256:", "sha-256="}) {
        if (starts_with(lower, prefix)) {
            t = trim(t.substr(std::string(prefix).size()));
            break;
        }
    }
    Sha256Digest d{};
    size_t n = 0;        // hex digits consumed
    bool sep_ok = false; // a separator may follow a complete pair only
    for (char c : t) {
        if (c == ':' || c == ' ' || c == '-') {
            if (!sep_ok) return false;
            sep_ok = false;
            continue;
        }
        const int v = hex_value(c);
        if (v < 0 || n >= 64) return false;
        d[n / 2] = uint8_t(d[n / 2] | (v << ((n % 2) ? 0 : 4)));
        ++n;
        sep_ok = (n % 2) == 0;
    }
    if (n != 64 || (!t.empty() && !sep_ok)) return false;
    out = d;
    return true;
}

} // namespace tn34xx
