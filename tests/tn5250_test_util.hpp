// SPDX-License-Identifier: GPL-3.0-or-later
// Shared helpers for the tn5250 engine tests: a fake host that feeds
// hand-written 5250 data streams and collects the engine's replies.
#pragma once

#include "tn5250/codepage.hpp"
#include "tn5250/engine.hpp"

#include <initializer_list>
#include <string>
#include <vector>

namespace tn5250_test {

using Bytes = std::vector<uint8_t>;
using namespace em34xx::tn5250;

// ASCII text to EBCDIC (CCSID 37).
inline Bytes eb(const std::string& s) {
    const CodePage* cp = CodePage::find("37");
    Bytes out;
    for (char c : s) {
        uint8_t b = 0x6F;
        cp->from_unicode(char32_t(static_cast<unsigned char>(c)), b);
        out.push_back(b);
    }
    return out;
}

inline Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

struct Host {
    Engine e;
    std::vector<Record> out;
    explicit Host(EngineOptions o = EngineOptions{}) : e(std::move(o)) {
        e.set_record_sink([this](const Record& r) { out.push_back(r); });
        e.set_system_available(true);
    }
    void write(const Bytes& d, uint8_t opcode = kOpPutGet) {
        Record r;
        r.opcode = opcode;
        r.data = d;
        e.receive(r);
    }
    // Row as ASCII: attributes and nulls as blanks, trailing blanks trimmed.
    std::string row(int r) const {
        const CodePage* cp = CodePage::find("37");
        std::string s;
        for (int c = 0; c < e.cols(); ++c) {
            const Cell& x = e.cell(r, c);
            char32_t u = U' ';
            if (!x.is_attribute() && x.ch >= 0x40 && x.ch != 0xFF) u = cp->to_unicode(x.ch);
            s += u < 0x80 ? char(u) : '?';
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
    void type(const std::string& s) {
        for (uint8_t b : eb(s)) e.key_data(b);
    }
    Record take() {
        Record r = out.front();
        out.erase(out.begin());
        return r;
    }
};

// A hand-written sign-on panel: two input fields (user: monocase,
// underscored; password: non-display), IC on the user field, then Read MDT.
inline Bytes signon_stream() {
    return cat({{0x04, 0x40},
                {0x04, 0x11, 0x00, 0x08},
                {0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                {0x11, 0x01, 0x24}, eb("Sign On"),
                {0x11, 0x06, 0x11}, eb("User"),
                {0x11, 0x06, 0x34, 0x1D, 0x40, 0x20, 0x24, 0x00, 0x0A},
                {0x11, 0x07, 0x11}, eb("Password"),
                {0x11, 0x07, 0x34, 0x1D, 0x40, 0x00, 0x27, 0x00, 0x0A},
                {0x13, 0x06, 0x35},
                {0x04, 0x52, 0x00, 0x00}});
}

// One input field with the given FFW at row r, col c (1-based attribute
// position), length len; keyboard unlocked; Read MDT armed.
inline Bytes one_field(uint8_t r, uint8_t c, uint16_t ffw, uint16_t len, std::initializer_list<uint16_t> fcws = {}) {
    Bytes sf{0x11, r, c, 0x1D, uint8_t(ffw >> 8), uint8_t(ffw)};
    for (uint16_t w : fcws) {
        sf.push_back(uint8_t(w >> 8));
        sf.push_back(uint8_t(w));
    }
    sf.insert(sf.end(), {0x24, uint8_t(len >> 8), uint8_t(len)});
    return cat({{0x04, 0x40, 0x04, 0x11, 0x00, 0x08}, sf, {0x04, 0x52, 0x00, 0x00}});
}

} // namespace tn5250_test
