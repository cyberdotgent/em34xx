// SPDX-License-Identifier: GPL-3.0-or-later
#include "util/strings.hpp"

#include <cctype>
#include <cstdarg>
#include <cstdio>

namespace em34xx {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t pos = s.find(sep, start);
        out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return out;
}

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_word = false, quoted = false;
    for (char c : s) {
        if (quoted) {
            if (c == '"') quoted = false;
            else cur += c;
        } else if (c == '"') {
            quoted = true;
            in_word = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (in_word) { out.push_back(cur); cur.clear(); in_word = false; }
        } else {
            cur += c;
            in_word = true;
        }
    }
    if (in_word) out.push_back(cur);
    return out;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool parse_uint(const std::string& text, uint64_t& out, int default_base) {
    std::string t = trim(text);
    int base = default_base;
    if (t.empty()) return false;
    if (t[0] == '#') { base = 10; t = t.substr(1); }
    else if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { base = 16; t = t.substr(2); }
    else if (t.size() > 1 && (t.back() == 'h' || t.back() == 'H')) { base = 16; t.pop_back(); }
    if (t.empty()) return false;
    uint64_t v = 0;
    for (char c : t) {
        if (c == '_') continue;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if (d >= base) return false;
        v = v * uint64_t(base) + uint64_t(d);
    }
    out = v;
    return true;
}

std::string format(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return {};
    if (size_t(n) < sizeof buf) return std::string(buf, size_t(n));
    std::string big(size_t(n) + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(&big[0], big.size(), fmt, ap);
    va_end(ap);
    big.resize(size_t(n));
    return big;
}

std::string hex_bytes(const uint8_t* data, size_t len) {
    std::string out;
    char b[4];
    for (size_t i = 0; i < len; ++i) {
        std::snprintf(b, sizeof b, i ? " %02x" : "%02x", data[i]);
        out += b;
    }
    return out;
}

} // namespace em34xx
