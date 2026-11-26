// SPDX-License-Identifier: GPL-3.0-or-later
// See engine.hpp. Codes and record formats are from the 3270 Data Stream
// Programmer's Reference (GA23-0059) [documented].
#include "tn3270/engine.hpp"

#include <algorithm>

namespace em34xx {
namespace tn3270 {

namespace {

// Commands (both the 3270 and the SNA codes) [documented].
bool is_write(uint8_t c) { return c == 0xF1 || c == 0x01; }
bool is_erase_write(uint8_t c) { return c == 0xF5 || c == 0x05; }
bool is_erase_write_alt(uint8_t c) { return c == 0x7E || c == 0x0D; }
bool is_read_buffer(uint8_t c) { return c == 0xF2 || c == 0x02; }
bool is_read_modified(uint8_t c) { return c == 0xF6 || c == 0x06; }
bool is_read_modified_all(uint8_t c) { return c == 0x6E || c == 0x0E; }
bool is_erase_all_unprotected(uint8_t c) { return c == 0x6F || c == 0x0F; }
bool is_write_structured_field(uint8_t c) { return c == 0xF3 || c == 0x11; }

// Orders [documented].
constexpr uint8_t kSF = 0x1D, kSFE = 0x29, kSBA = 0x11, kSA = 0x28, kMF = 0x2C, kIC = 0x13, kPT = 0x05,
                  kRA = 0x3C, kEUA = 0x12, kGE = 0x08;

// WCC bits [documented].
constexpr uint8_t kWccReset = 0x40, kWccAlarm = 0x04, kWccRestore = 0x02, kWccResetMdt = 0x01;

// Extended attribute types [documented].
constexpr uint8_t kAttrAll = 0x00, kAttrField = 0xC0, kAttrHighlight = 0x41, kAttrColour = 0x42,
                  kAttrCharset = 0x43;

// The 6-bit buffer address code [documented].
constexpr uint8_t kAddrCode[64] = {
    0x40, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
    0x50, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
    0x60, 0x61, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
};

constexpr uint8_t kDup = 0x1C, kFieldMark = 0x1E;

bool numeric_ok(uint8_t e) { return (e >= 0xF0 && e <= 0xF9) || e == 0x60 || e == 0x4B || e == kDup; }

} // namespace

uint8_t pf_aid(int n) {
    static constexpr uint8_t kPf[24] = {0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0x7A, 0x7B, 0x7C,
                                        0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0x4A, 0x4B, 0x4C};
    return n >= 1 && n <= 24 ? kPf[n - 1] : uint8_t(Aid::None);
}

void encode_address(int addr, uint8_t& b1, uint8_t& b2) {
    if (addr < 4096) {
        b1 = kAddrCode[(addr >> 6) & 0x3F];
        b2 = kAddrCode[addr & 0x3F];
    } else {   // 14-bit binary
        b1 = uint8_t((addr >> 8) & 0x3F);
        b2 = uint8_t(addr);
    }
}

int decode_address(uint8_t b1, uint8_t b2) {
    if ((b1 & 0xC0) == 0) return ((b1 & 0x3F) << 8) | b2;   // 14-bit
    return ((b1 & 0x3F) << 6) | (b2 & 0x3F);               // 12-bit
}

Engine::Engine(int alt_rows, int alt_cols) : alt_rows_(alt_rows), alt_cols_(alt_cols) { erase(24, 80); }

void Engine::erase(int rows, int cols) {
    rows_ = rows;
    cols_ = cols;
    buf_.assign(size_t(rows * cols), Cell());
    cursor_ = 0;
    touch();
}

bool Engine::formatted() const {
    return std::any_of(buf_.begin(), buf_.end(), [](const Cell& c) { return c.fa; });
}

int Engine::field_start(int addr) const {
    const int n = size();
    for (int i = 0; i < n; ++i) {
        const int a = (addr - i + n) % n;
        if (buf_[size_t(a)].fa) return a;
    }
    return -1;
}

uint8_t Engine::field_attr(int addr) const {
    const int f = field_start(addr);
    return f < 0 ? 0 : buf_[size_t(f)].ch;
}

int Engine::next_unprotected(int from) const {
    // The first character position of the next unprotected field after
    // `from` (wrapping), or -1.
    const int n = size();
    for (int i = 1; i <= n; ++i) {
        const int a = (from + i) % n;
        if (buf_[size_t(a)].fa && !fa_protected(buf_[size_t(a)].ch)) {
            const int first = (a + 1) % n;
            if (!buf_[size_t(first)].fa) return first;
        }
    }
    return -1;
}

void Engine::set_mdt(int addr) {
    const int f = field_start(addr);
    if (f >= 0) buf_[size_t(f)].ch |= 0x01;
}

void Engine::apply_wcc(uint8_t wcc) {
    if (wcc & kWccResetMdt)
        for (Cell& c : buf_)
            if (c.fa) c.ch &= uint8_t(~0x01);
    if (wcc & kWccAlarm) ++alarms_;
    if (wcc & kWccRestore) {
        locked_ = false;
        last_aid_ = uint8_t(Aid::None);
    }
    (void)kWccReset;
}

void Engine::process(const std::vector<uint8_t>& rec, std::vector<std::vector<uint8_t>>& out) {
    if (rec.empty()) return;
    const uint8_t cmd = rec[0];
    if (is_write(cmd)) write(rec.data() + 1, rec.size() - 1, false, rows_, cols_);
    else if (is_erase_write(cmd)) write(rec.data() + 1, rec.size() - 1, true, 24, 80);
    else if (is_erase_write_alt(cmd)) write(rec.data() + 1, rec.size() - 1, true, alt_rows_, alt_cols_);
    else if (is_read_buffer(cmd)) read_buffer(out);
    else if (is_read_modified(cmd)) read_modified(last_aid_, false, out);
    else if (is_read_modified_all(cmd)) read_modified(last_aid_, true, out);
    else if (is_erase_all_unprotected(cmd)) {
        int first = -1;
        for (int a = 0; a < size(); ++a) {
            const uint8_t fa = field_attr(a);
            Cell& c = buf_[size_t(a)];
            if (c.fa) {
                if (!fa_protected(c.ch)) c.ch &= uint8_t(~0x01);
                continue;
            }
            if (!fa_protected(fa)) {
                c.ch = 0;
                if (first < 0) first = a;
            }
        }
        cursor_ = first < 0 ? 0 : first;
        locked_ = false;
        last_aid_ = uint8_t(Aid::None);
        touch();
    } else if (is_write_structured_field(cmd)) structured_fields(rec.data() + 1, rec.size() - 1, out);
}

void Engine::write(const uint8_t* p, size_t n, bool erase_first, int rows, int cols) {
    if (erase_first) erase(rows, cols);
    if (n == 0) return;
    const uint8_t wcc = p[0];
    size_t i = 1;
    int addr = erase_first ? 0 : cursor_;
    const int sz = size();
    // Extended attributes the next characters take (SA) [documented].
    uint8_t sa_hl = 0, sa_col = 0, sa_cs = 0;
    auto put = [&](uint8_t ch, uint8_t cs) {
        Cell& c = buf_[size_t(addr)];
        c = Cell();
        c.ch = ch;
        c.highlight = sa_hl;
        c.colour = sa_col;
        c.charset = cs;
        addr = (addr + 1) % sz;
    };
    while (i < n) {
        const uint8_t b = p[i++];
        switch (b) {
        case kSF:
            if (i < n) {
                Cell& c = buf_[size_t(addr)];
                c = Cell();
                c.fa = true;
                c.ch = uint8_t(p[i++] & 0x3F);
                addr = (addr + 1) % sz;
            }
            break;
        case kSFE: {
            if (i >= n) break;
            const unsigned pairs = p[i++];
            Cell c;
            c.fa = true;
            for (unsigned k = 0; k < pairs && i + 2 <= n; ++k) {
                const uint8_t t = p[i], v = p[i + 1];
                i += 2;
                if (t == kAttrField) c.ch = uint8_t(v & 0x3F);
                else if (t == kAttrHighlight) c.highlight = v;
                else if (t == kAttrColour) c.colour = v;
                else if (t == kAttrCharset) c.charset = v;
            }
            buf_[size_t(addr)] = c;
            addr = (addr + 1) % sz;
            break;
        }
        case kSBA:
            if (i + 2 <= n) {
                addr = decode_address(p[i], p[i + 1]) % sz;
                i += 2;
            } else {
                i = n;
            }
            break;
        case kSA:
            if (i + 2 <= n) {
                const uint8_t t = p[i], v = p[i + 1];
                i += 2;
                if (t == kAttrAll) sa_hl = sa_col = sa_cs = 0;
                else if (t == kAttrHighlight) sa_hl = v;
                else if (t == kAttrColour) sa_col = v;
                else if (t == kAttrCharset) sa_cs = v;
            } else {
                i = n;
            }
            break;
        case kMF: {
            if (i >= n) break;
            const unsigned pairs = p[i++];
            Cell& c = buf_[size_t(addr)];
            for (unsigned k = 0; k < pairs && i + 2 <= n; ++k) {
                const uint8_t t = p[i], v = p[i + 1];
                i += 2;
                if (!c.fa) continue;
                if (t == kAttrField) c.ch = uint8_t(v & 0x3F);
                else if (t == kAttrHighlight) c.highlight = v;
                else if (t == kAttrColour) c.colour = v;
                else if (t == kAttrCharset) c.charset = v;
            }
            addr = (addr + 1) % sz;
            break;
        }
        case kIC: cursor_ = addr; break;
        case kPT: {
            const int nx = next_unprotected(addr - 1 < 0 ? sz - 1 : addr - 1);
            // Tab within a write: to the next unprotected field's first
            // position (null-filling is not applied here).
            addr = nx < 0 ? 0 : nx;
            break;
        }
        case kRA:
            if (i + 3 <= n) {
                const int to = decode_address(p[i], p[i + 1]) % sz;
                i += 2;
                uint8_t ch = p[i++], cs = sa_cs;
                if (ch == kGE && i < n) {
                    ch = p[i++];
                    cs = 0xF1;
                }
                do put(ch, cs);
                while (addr != to);
            } else {
                i = n;
            }
            break;
        case kEUA:
            if (i + 2 <= n) {
                const int to = decode_address(p[i], p[i + 1]) % sz;
                i += 2;
                do {
                    Cell& c = buf_[size_t(addr)];
                    if (!c.fa && !fa_protected(field_attr(addr))) c = Cell();
                    addr = (addr + 1) % sz;
                } while (addr != to);
            } else {
                i = n;
            }
            break;
        case kGE:
            if (i < n) put(p[i++], 0xF1);
            break;
        default: put(b, sa_cs); break;
        }
    }
    apply_wcc(wcc);
    touch();
}

void Engine::read_buffer(std::vector<std::vector<uint8_t>>& out) const {
    std::vector<uint8_t> r;
    r.push_back(last_aid_);
    uint8_t b1, b2;
    encode_address(cursor_, b1, b2);
    r.push_back(b1);
    r.push_back(b2);
    for (const Cell& c : buf_) {
        if (c.fa) {
            r.push_back(kSF);
            r.push_back(kAddrCode[c.ch & 0x3F]);
        } else {
            r.push_back(c.ch);
        }
    }
    out.push_back(std::move(r));
}

void Engine::read_modified(uint8_t aid, bool all, std::vector<std::vector<uint8_t>>& out) const {
    std::vector<uint8_t> r;
    r.push_back(aid);
    // A short read for PA keys and Clear [documented].
    const bool short_read = !all && (aid == uint8_t(Aid::PA1) || aid == uint8_t(Aid::PA2) ||
                                     aid == uint8_t(Aid::PA3) || aid == uint8_t(Aid::Clear));
    if (short_read) {
        out.push_back(std::move(r));
        return;
    }
    uint8_t b1, b2;
    encode_address(cursor_, b1, b2);
    r.push_back(b1);
    r.push_back(b2);
    const int sz = size();
    if (!formatted()) {
        // Unformatted: the whole buffer's data, nulls suppressed.
        for (const Cell& c : buf_)
            if (c.ch) r.push_back(c.ch);
        out.push_back(std::move(r));
        return;
    }
    for (int a = 0; a < sz; ++a) {
        const Cell& f = buf_[size_t(a)];
        if (!f.fa || !fa_modified(f.ch)) continue;
        const int first = (a + 1) % sz;
        encode_address(first, b1, b2);
        r.push_back(kSBA);
        r.push_back(b1);
        r.push_back(b2);
        for (int k = first; !buf_[size_t(k)].fa; k = (k + 1) % sz) {
            if (buf_[size_t(k)].ch) r.push_back(buf_[size_t(k)].ch);
            if ((k + 1) % sz == a) break;
        }
    }
    out.push_back(std::move(r));
}

std::vector<uint8_t> Engine::query_reply() const {
    // AID 88 followed by query reply structured fields [documented]:
    // Summary, Usable Area, Implicit Partition, Colour, Highlighting, Reply
    // Modes.
    std::vector<uint8_t> r{0x88};
    auto sf = [&](std::initializer_list<uint8_t> body) {
        const size_t len = body.size() + 2;
        r.push_back(uint8_t(len >> 8));
        r.push_back(uint8_t(len));
        r.insert(r.end(), body.begin(), body.end());
    };
    sf({0x81, 0x80, 0x80, 0x81, 0x86, 0x87, 0x88, 0xA6});
    const uint8_t w = uint8_t(alt_cols_), h = uint8_t(alt_rows_);
    const unsigned cells = unsigned(alt_rows_ * alt_cols_);
    sf({0x81, 0x81, 0x01, 0x00, 0x00, w, 0x00, h, 0x01, 0x00, 0x0A, 0x02, 0xE5, 0x00, 0x02, 0x00, 0x6F, 0x09,
        0x0C, uint8_t(cells >> 8), uint8_t(cells)});
    sf({0x81, 0xA6, 0x00, 0x00, 0x0B, 0x01, 0x00, 0x00, 0x50, 0x00, 0x18, 0x00, w, 0x00, h});
    sf({0x81, 0x86, 0x00, 0x08, 0x00, 0xF4, 0xF1, 0xF1, 0xF2, 0xF2, 0xF3, 0xF3, 0xF4, 0xF4, 0xF5, 0xF5, 0xF6,
        0xF6, 0xF7, 0xF7});
    sf({0x81, 0x87, 0x05, 0x00, 0xF0, 0xF1, 0xF1, 0xF2, 0xF2, 0xF4, 0xF4, 0xF8, 0xF8});
    sf({0x81, 0x88, 0x00, 0x01, 0x02});
    return r;
}

void Engine::structured_fields(const uint8_t* p, size_t n, std::vector<std::vector<uint8_t>>& out) {
    size_t i = 0;
    while (i + 3 <= n) {
        size_t len = size_t(p[i]) << 8 | p[i + 1];
        if (len == 0) len = n - i;   // the last field may give length 0
        if (len < 3 || i + len > n) break;
        const uint8_t id = p[i + 2];
        const uint8_t* body = p + i + 3;
        const size_t blen = len - 3;
        if (id == 0x01 && blen >= 2) {          // Read Partition
            const uint8_t type = body[1];
            if (type == 0x02 || type == 0x03)   // Query, Query List
                out.push_back(query_reply());
            else if (type == 0xF2) read_buffer(out);
            else if (type == 0xF6) read_modified(last_aid_, false, out);
            else if (type == 0x6E) read_modified(last_aid_, true, out);
        } else if (id == 0x40 && blen >= 2) {   // Outbound 3270DS: pid, command, data
            const uint8_t cmd = body[1];
            std::vector<uint8_t> rec(body + 1, body + blen);
            if (is_write(cmd) || is_erase_write(cmd) || is_erase_write_alt(cmd) || is_erase_all_unprotected(cmd))
                process(rec, out);
        }
        i += len;
    }
}

// --- operator side -------------------------------------------------------------

bool Engine::type(uint8_t e) {
    if (locked_) return false;
    const int sz = size();
    Cell& c = buf_[size_t(cursor_)];
    const uint8_t fa = field_attr(cursor_);
    if (c.fa || (formatted() && fa_protected(fa)) || (formatted() && fa_numeric(fa) && !numeric_ok(e))) {
        opererr_ = true;
        touch();
        return false;
    }
    if (insert_) {
        // Shift the rest of the field right; there must be a null at its end.
        int end = cursor_;
        while (!buf_[size_t((end + 1) % sz)].fa && (end + 1) % sz != cursor_) end = (end + 1) % sz;
        if (buf_[size_t(end)].ch != 0) {
            opererr_ = true;
            touch();
            return false;
        }
        for (int a = end; a != cursor_; a = (a - 1 + sz) % sz) buf_[size_t(a)] = buf_[size_t((a - 1 + sz) % sz)];
    }
    c = Cell();
    c.ch = e;
    set_mdt(cursor_);
    cursor_ = (cursor_ + 1) % sz;
    // Skip to the next unprotected field when the cursor lands on an attribute.
    if (buf_[size_t(cursor_)].fa) {
        const int nx = next_unprotected(cursor_ - 1 < 0 ? sz - 1 : cursor_ - 1);
        if (nx >= 0 && fa_protected(buf_[size_t(cursor_)].ch) == false) cursor_ = (cursor_ + 1) % sz;
        else if (nx >= 0) cursor_ = nx;
    }
    touch();
    return true;
}

void Engine::key(Key k) {
    const int sz = size();
    if (k == Key::Reset) {
        locked_ = false;
        insert_ = false;
        opererr_ = false;
        touch();
        return;
    }
    if (locked_) return;
    switch (k) {
    case Key::Tab: {
        const int nx = next_unprotected(cursor_);
        cursor_ = nx < 0 ? 0 : nx;
        break;
    }
    case Key::BackTab: {
        // To the start of this field, or of the previous unprotected one.
        int a = cursor_;
        for (int i = 0; i < sz; ++i) {
            a = (a - 1 + sz) % sz;
            if (buf_[size_t(a)].fa && !fa_protected(buf_[size_t(a)].ch) && (a + 1) % sz != cursor_) {
                cursor_ = (a + 1) % sz;
                break;
            }
        }
        break;
    }
    case Key::Home: {
        const int nx = next_unprotected(sz - 1);
        cursor_ = nx < 0 ? 0 : nx;
        break;
    }
    case Key::NewLine: {
        const int line_end = (cursor_ / cols_ + 1) * cols_ - 1;
        const int nx = next_unprotected(line_end % sz);
        cursor_ = nx < 0 ? ((cursor_ / cols_ + 1) * cols_) % sz : nx;
        break;
    }
    case Key::Left: cursor_ = (cursor_ - 1 + sz) % sz; break;
    case Key::Right: cursor_ = (cursor_ + 1) % sz; break;
    case Key::Up: cursor_ = (cursor_ - cols_ + sz) % sz; break;
    case Key::Down: cursor_ = (cursor_ + cols_) % sz; break;
    case Key::Backspace: cursor_ = (cursor_ - 1 + sz) % sz; break;
    case Key::Insert: insert_ = !insert_; break;
    case Key::Delete: {
        if (buf_[size_t(cursor_)].fa || fa_protected(field_attr(cursor_))) {
            opererr_ = true;
            break;
        }
        int a = cursor_;
        while (!buf_[size_t((a + 1) % sz)].fa && (a + 1) % sz != cursor_) {
            buf_[size_t(a)] = buf_[size_t((a + 1) % sz)];
            a = (a + 1) % sz;
        }
        buf_[size_t(a)] = Cell();
        set_mdt(cursor_);
        break;
    }
    case Key::EraseEOF:
        if (buf_[size_t(cursor_)].fa || fa_protected(field_attr(cursor_))) {
            opererr_ = true;
            break;
        }
        for (int a = cursor_; !buf_[size_t(a)].fa; a = (a + 1) % sz) {
            buf_[size_t(a)] = Cell();
            if ((a + 1) % sz == cursor_) break;
        }
        set_mdt(cursor_);
        break;
    case Key::EraseInput: {
        for (int a = 0; a < sz; ++a) {
            Cell& c = buf_[size_t(a)];
            if (c.fa) {
                if (!fa_protected(c.ch)) c.ch &= uint8_t(~0x01);
            } else if (formatted() && !fa_protected(field_attr(a))) {
                c = Cell();
            }
        }
        const int nx = next_unprotected(sz - 1);
        cursor_ = nx < 0 ? 0 : nx;
        break;
    }
    case Key::Dup:
        type(kDup);
        return;
    case Key::FieldMark:
        type(kFieldMark);
        return;
    case Key::Reset: break;
    }
    touch();
}

void Engine::aid(uint8_t a, std::vector<std::vector<uint8_t>>& out) {
    if (locked_ && a != uint8_t(Aid::SysReq)) return;
    last_aid_ = a;
    if (a == uint8_t(Aid::Clear)) {
        erase(24, 80);
        last_aid_ = a;
    }
    read_modified(a, false, out);
    locked_ = true;
    insert_ = false;
    touch();
}

} // namespace tn3270
} // namespace em34xx
