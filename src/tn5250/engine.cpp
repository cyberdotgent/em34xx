// SPDX-License-Identifier: GPL-3.0-or-later
#include "tn5250/engine.hpp"

#include "util/log.hpp"

#include <algorithm>

namespace em34xx {
namespace tn5250 {

namespace {

constexpr uint8_t kNull = 0x00;
constexpr uint8_t kBlank = 0x40;
constexpr uint8_t kMinus = 0x60;
constexpr uint8_t kDupChar = 0x1C;
constexpr uint8_t kFieldMarkChar = 0x1E;

bool is_digit(uint8_t c) { return c >= 0xF0 && c <= 0xF9; }
bool is_lower(uint8_t c) {
    return (c >= 0x81 && c <= 0x89) || (c >= 0x91 && c <= 0x99) || (c >= 0xA2 && c <= 0xA9);
}
bool is_upper(uint8_t c) {
    return (c >= 0xC1 && c <= 0xC9) || (c >= 0xD1 && c <= 0xD9) || (c >= 0xE2 && c <= 0xE9);
}
uint8_t to_upper(uint8_t c) { return is_lower(c) ? uint8_t(c + 0x40) : c; }

// Invariant EBCDIC for the Query Reply's machine type and model (digits and
// A-Z have the same code points in every single-byte EBCDIC code page).
uint8_t ebcdic_invariant(char ch) {
    if (ch >= '0' && ch <= '9') return uint8_t(0xF0 + (ch - '0'));
    if (ch >= 'A' && ch <= 'I') return uint8_t(0xC1 + (ch - 'A'));
    if (ch >= 'J' && ch <= 'R') return uint8_t(0xD1 + (ch - 'J'));
    if (ch >= 'S' && ch <= 'Z') return uint8_t(0xE2 + (ch - 'S'));
    return kBlank;
}

void put16(std::vector<uint8_t>& v, unsigned x) {
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

// PackBits-style run-length coding for the save image: a header byte h,
// then either h+1 literal bytes (h < 0x80) or one byte repeated
// (h & 0x7F) + 1 times.
void rle_encode(std::vector<uint8_t>& out, const std::vector<uint8_t>& in) {
    size_t i = 0;
    while (i < in.size()) {
        size_t run = 1;
        while (i + run < in.size() && run < 128 && in[i + run] == in[i]) ++run;
        if (run >= 3) {
            out.push_back(uint8_t(0x80 | (run - 1)));
            out.push_back(in[i]);
            i += run;
            continue;
        }
        size_t start = i, n = 0;
        while (i < in.size() && n < 128) {
            if (i + 2 < in.size() && in[i] == in[i + 1] && in[i] == in[i + 2]) break;
            ++i;
            ++n;
        }
        out.push_back(uint8_t(n - 1));
        out.insert(out.end(), in.begin() + long(start), in.begin() + long(start + n));
    }
}

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    size_t pos() const { return i_; }
    uint8_t u8() {
        if (i_ >= n_) { ok_ = false; return 0; }
        return p_[i_++];
    }
    unsigned u16() {
        unsigned hi = u8();
        return (hi << 8) | u8();
    }
    bool rle(std::vector<uint8_t>& out, size_t count) {
        out.clear();
        while (ok_ && out.size() < count) {
            const uint8_t h = u8();
            if (h & 0x80) {
                const uint8_t v = u8();
                out.insert(out.end(), size_t(h & 0x7F) + 1, v);
            } else {
                for (unsigned k = 0; k <= h; ++k) out.push_back(u8());
            }
        }
        return ok_ && out.size() == count;
    }

private:
    const uint8_t* p_;
    size_t n_;
    size_t i_ = 0;
    bool ok_ = true;
};

constexpr uint8_t kImageVersion = 1;

} // namespace

EngineOptions EngineOptions::for_terminal_type(const std::string& ttype) {
    EngineOptions o;
    // RFC 1205 terminal types look like IBM-tttt-mm.
    std::string type, model;
    if (ttype.size() > 4 && ttype.compare(0, 4, "IBM-") == 0) {
        const size_t dash = ttype.find('-', 4);
        type = ttype.substr(4, dash == std::string::npos ? std::string::npos : dash - 4);
        if (dash != std::string::npos) model = ttype.substr(dash + 1);
    }
    if (type.size() == 4) o.machine_type = type;
    while (model.size() < 3) model.insert(model.begin(), '0');
    o.model = model.substr(model.size() - 3);
    // Only claim what the device really has: every 3477 does 27x132 and
    // only the FC is colour (spec-3477-device-protocol.md, PAI 16.0).
    o.wide = o.machine_type == "3477";
    o.color = o.machine_type == "3477" && o.model == "0FC";
    return o;
}

bool Field::transparent() const { return fcw(0x84) >= 0; }

int Field::fcw(uint8_t type) const {
    for (uint16_t w : fcws)
        if ((w >> 8) == type) return w & 0xFF;
    return -1;
}

Engine::Engine(EngineOptions options) : opt_(std::move(options)) {
    cells_.assign(size_t(size()), Cell{});
    dirty_.add(0, 0);
    dirty_.add(rows_ - 1, cols_ - 1);
}

// ---------------------------------------------------------------------------
// Screen primitives

void Engine::set_ch(int addr, uint8_t ch) {
    Cell& c = cells_[size_t(addr)];
    if (c.ch == ch) return;
    c.ch = ch;
    dirty_.add(addr / cols_, addr % cols_);
    changed();
}

void Engine::set_ext(int addr, uint8_t primary, uint8_t color) {
    Cell& c = cells_[size_t(addr)];
    if (c.ext_primary == primary && c.ext_color == color) return;
    c.ext_primary = primary;
    c.ext_color = color;
    dirty_.add(addr / cols_, addr % cols_);
    changed();
}

void Engine::set_cursor(int addr) {
    if (addr < 0 || addr >= size()) addr = 0;
    if (cursor_ != addr) {
        cursor_ = addr;
        changed();
    }
}

void Engine::refresh_attrs() {
    uint8_t cur = 0x20;
    for (int a = 0; a < size(); ++a) {
        Cell& c = cells_[size_t(a)];
        if (c.is_attribute()) cur = c.ch;
        if (c.attr != cur) {
            c.attr = cur;
            dirty_.add(a / cols_, a % cols_);
            changed();
        }
    }
}

Indicators Engine::indicators() const {
    Indicators i;
    i.system_available = system_available_;
    i.message_waiting = message_waiting_;
    i.keyboard_locked = locked_;
    i.input_inhibited = locked_ || error_ != ErrorState::None;
    i.insert_mode = insert_;
    i.cursor_blink = blink_ || error_ != ErrorState::None;
    i.error = error_;
    i.error_code = error_code_;
    i.clicker = !locked_ && error_ == ErrorState::None;
    return i;
}

Rect Engine::take_dirty() {
    Rect r = dirty_;
    dirty_ = Rect{};
    return r;
}

unsigned Engine::take_alarms() {
    unsigned n = alarms_;
    alarms_ = 0;
    return n;
}

void Engine::snapshot(ScreenSnapshot& out) const {
    out.rows = rows_;
    out.cols = cols_;
    out.cells = cells_;
    out.cursor_row = cursor_row();
    out.cursor_col = cursor_col();
    out.indicators = indicators();
    out.generation = generation_;
}

void Engine::set_device_ready(bool ready) {
    if (ready && !device_ready_) {
        dirty_.add(0, 0);
        dirty_.add(rows_ - 1, cols_ - 1);
        changed();
    }
    device_ready_ = ready;
}

void Engine::set_system_available(bool on) {
    if (system_available_ == on) return;
    system_available_ = on;
    if (!on) locked_ = true;
    changed();
}

int Engine::error_row() const {
    if (error_row_header_ >= 1 && error_row_header_ <= rows_) return error_row_header_ - 1;
    return rows_ - 1;
}

// ---------------------------------------------------------------------------
// Records and commands

void Engine::send(uint8_t opcode, uint8_t flags, std::vector<uint8_t> data) {
    if (!sink_) return;
    Record r;
    r.opcode = opcode;
    r.flags = flags;
    r.data = std::move(data);
    sink_(r);
}

void Engine::send_negative(uint32_t sense) {
    TN_LOG("tn5250", Warn, "negative response %08X", sense);
    send(kOpNoOp, kFlagErr,
         {uint8_t(sense >> 24), uint8_t(sense >> 16), uint8_t(sense >> 8), uint8_t(sense)});
}

void Engine::receive(const Record& rec) {
    switch (rec.opcode) {
    case kOpMessageOn:
        if (!message_waiting_) { message_waiting_ = true; changed(); }
        break;
    case kOpMessageOff:
        if (message_waiting_) { message_waiting_ = false; changed(); }
        break;
    case kOpCancelInvite:
        // RFC 1205 4.2: acknowledge, and stop being invited.
        read_cmd_ = 0;
        send(kOpCancelInvite, 0, {});
        break;
    default: break;
    }
    if (!rec.data.empty()) process_stream(rec.data, rec.opcode);
}

void Engine::process_stream(const std::vector<uint8_t>& d, uint8_t opcode) {
    (void)opcode;
    size_t i = 0;
    auto need = [&](size_t n) {
        if (i + n > d.size()) throw Error{kSensePrematureEnd};
    };
    try {
        while (i < d.size()) {
            if (d[i] != 0x04) throw Error{kSenseNoEscape};
            need(2);
            const uint8_t cmd = d[i + 1];
            i += 2;
            switch (cmd) {
            case 0x40: cmd_clear_unit(24, 80); break;
            case 0x20: {  // Clear Unit Alternate (FR 15.4)
                need(1);
                const uint8_t p = d[i++];
                if (p == 0x00) {
                    if (!opt_.wide) throw Error{kSenseInvalidCommand};
                    cmd_clear_unit(27, 132);
                } else if (p == 0x80) {
                    cmd_clear_unit(rows_, cols_);
                } else {
                    throw Error{kSenseInvalidClearAlt};
                }
                break;
            }
            case 0x50: cmd_clear_format_table(); break;
            case 0x11: cmd_write_to_display(d, i); break;
            case 0x21: cmd_write_error_code(d, i, false); break;
            case 0x22: cmd_write_error_code(d, i, true); break;
            case 0x42:
            case 0x52:
            case 0x82:
                need(2);
                cmd_read(cmd, d[i], d[i + 1]);
                i += 2;
                break;
            case 0x72:
            case 0x83:
                send(kOpNoOp, 0, build_read_reply(cmd, 0x00, true));
                break;
            case 0x62: {  // Read Screen (FR 15.13): the buffer, raw
                std::vector<uint8_t> out;
                out.reserve(cells_.size());
                for (const Cell& c : cells_) out.push_back(c.ch);
                send(kOpNoOp, 0, std::move(out));
                break;
            }
            case 0x02: {  // Save Screen (FR 15.21): reply starts 04 12
                std::vector<uint8_t> out{0x04, 0x12};
                const auto img = save_image();
                out.insert(out.end(), img.begin(), img.end());
                send(kOpSaveScreen, 0, std::move(out));
                break;
            }
            case 0x03: {  // Save Partial Screen (FR 15.22): whole screen, 04 13 LL
                need(5);
                i += 5;
                const auto img = save_image();
                std::vector<uint8_t> out{0x04, 0x13};
                put16(out, unsigned(img.size() + 2));
                out.insert(out.end(), img.begin(), img.end());
                send(kOpSaveScreen, 0, std::move(out));
                break;
            }
            case 0x12: {  // Restore Screen (FR 15.23)
                size_t used = 0;
                if (!restore_image(d.data() + i, d.size() - i, used)) throw Error{kSenseRestoreData};
                i += used;
                break;
            }
            case 0x13: {  // Restore Partial Screen: LL includes itself
                need(2);
                const size_t ll = (size_t(d[i]) << 8) | d[i + 1];
                if (ll < 2 || i + ll > d.size()) throw Error{kSenseRestoreData};
                size_t used = 0;
                if (!restore_image(d.data() + i + 2, ll - 2, used)) throw Error{kSenseRestoreData};
                i += ll;
                break;
            }
            case 0x23:  // Roll (FR 15.25)
                need(3);
                cmd_roll(d[i], d[i + 1], d[i + 2]);
                i += 3;
                break;
            case 0xF3: cmd_write_structured_field(d, i); break;
            case 0xF4: {  // Write Single Structured Field: nothing we support
                need(2);
                const size_t ll = (size_t(d[i]) << 8) | d[i + 1];
                if (ll < 4) throw Error{kSenseSfLength};
                need(ll);
                TN_LOG("tn5250", Info, "WSSF class %02X type %02X ignored", d[i + 2], d[i + 3]);
                i += ll;
                break;
            }
            default:
                TN_LOG("tn5250", Warn, "unsupported command %02X", cmd);
                throw Error{kSenseInvalidCommand};
            }
        }
    } catch (const Error& e) {
        send_negative(e.sense);
    }
    refresh_attrs();
}

void Engine::cmd_clear_unit(int r, int c) {
    rows_ = r;
    cols_ = c;
    cells_.assign(size_t(size()), Cell{});
    fields_.clear();
    soh_flags_ = 0;
    resequence_ = 0;
    error_row_header_ = 0;
    pf_no_data_ = 0;
    ic_ = -1;
    cursor_ = 0;
    locked_ = true;
    insert_ = false;
    blink_ = false;
    error_ = ErrorState::None;
    error_code_ = 0;
    unlock_after_reset_ = false;
    saved_error_line_.clear();
    fer_wait_ = false;
    read_cmd_ = 0;
    pending_aid_ = 0;
    dirty_ = Rect{};
    dirty_.add(0, 0);
    dirty_.add(rows_ - 1, cols_ - 1);
    changed();
}

void Engine::cmd_clear_format_table() {
    fields_.clear();
    soh_flags_ = 0;
    resequence_ = 0;
    pf_no_data_ = 0;
    locked_ = true;
    insert_ = false;
    read_cmd_ = 0;
    pending_aid_ = 0;
    fer_wait_ = false;
    changed();
}

void Engine::apply_cc0(uint8_t cc0) {
    const unsigned v = cc0 >> 5;  // FR Table 43
    if (v == 0) return;
    pending_aid_ = 0;
    lock_keyboard();
    for (Field& f : fields_) {
        const bool nb = !f.bypass();
        bool null_it = false;
        switch (v) {
        case 4: null_it = nb && f.mdt(); break;
        case 5: null_it = nb; break;
        case 6: null_it = nb && f.mdt(); break;
        case 7: null_it = nb; break;
        default: break;
        }
        if (null_it)
            for (int a = f.start; a < f.end(); ++a) set_ch(a, kNull);
        bool reset = false;
        switch (v) {
        case 2: case 5: case 6: reset = nb; break;
        case 3: case 7: reset = true; break;
        default: break;
        }
        if (reset) f.ffw = uint16_t(f.ffw & ~0x0800);
    }
}

void Engine::apply_cc1(uint8_t cc1, int cursor_order) {
    // FR Table 44.
    if (cc1 & 0x08) {
        if (error_ != ErrorState::None) {
            unlock_after_reset_ = true;  // deferred until Error Reset
        } else if (locked_) {
            locked_ = false;
            pending_aid_ = 0;
            if (!(cc1 & 0x40)) set_cursor(home_address());
            changed();
        }
    }
    if (cursor_order >= 0) set_cursor(cursor_order);
    if (cc1 & 0x20) blink_ = false;
    if (cc1 & 0x10) blink_ = true;
    if (cc1 & 0x04) ++alarms_;
    if ((cc1 & 0x03) == 0x02) message_waiting_ = false;
    else if (cc1 & 0x01) message_waiting_ = true;
    changed();
}

void Engine::cmd_write_to_display(const std::vector<uint8_t>& d, size_t& i) {
    auto need = [&](size_t n) {
        if (i + n > d.size()) throw Error{kSensePrematureEnd};
    };
    auto address = [&](uint8_t r, uint8_t c) {
        if (r < 1 || r > rows_ || c < 1 || c > cols_) throw Error{kSenseRowCol};
        return (r - 1) * cols_ + (c - 1);
    };
    need(2);
    const uint8_t cc0 = d[i], cc1 = d[i + 1];
    i += 2;
    apply_cc0(cc0);

    int addr = cursor_;
    int mc = -1;              // MC address if MC was the last cursor order
    bool sf_follow = false;   // an SF just ended; the next SF starts after it
    int sf_next = 0;

    while (i < d.size() && d[i] != 0x04) {
        const uint8_t b = d[i++];
        const bool after_sf = sf_follow;
        sf_follow = false;
        switch (b) {
        case 0x11: {  // SBA (FR 15.6.4); row r column 0 only before an SF
            need(2);
            const uint8_t r = d[i], c = d[i + 1];
            i += 2;
            if (c == 0 && r >= 1 && r <= rows_ && i < d.size() && d[i] == 0x1D) addr = (r - 1) * cols_ - 1;
            else addr = address(r, c);
            break;
        }
        case 0x13:  // IC (FR 15.6.5)
            need(2);
            ic_ = address(d[i], d[i + 1]);
            i += 2;
            mc = -1;
            break;
        case 0x14:  // MC (RFC 1205 5.3)
            need(2);
            mc = address(d[i], d[i + 1]);
            i += 2;
            break;
        case 0x02: {  // RA (FR 15.6.7)
            need(3);
            const int t = address(d[i], d[i + 1]);
            const uint8_t ch = d[i + 2];
            i += 3;
            if (addr < 0 || t < addr) throw Error{kSenseRaBackwards};
            for (int a = addr; a <= t; ++a) set_ch(a, ch);
            addr = t + 1;
            break;
        }
        case 0x03: {  // EA (FR 15.6.8)
            need(3);
            const int t = address(d[i], d[i + 1]);
            const uint8_t ll = d[i + 2];
            i += 3;
            if (ll < 2 || ll > 5) throw Error{kSenseExtAttrType};
            need(size_t(ll - 1));
            bool screen = false, primary = false, color = false;
            for (int k = 0; k < ll - 1; ++k) {
                switch (d[i + size_t(k)]) {
                case 0x00: screen = true; break;
                case 0x01: primary = true; break;
                case 0x03: color = true; break;
                case 0xFF: screen = primary = color = true; break;
                default: throw Error{kSenseExtAttrType};
                }
            }
            i += size_t(ll - 1);
            if (addr < 0 || t < addr) throw Error{kSenseRaBackwards};
            for (int a = addr; a <= t; ++a) {
                if (screen) set_ch(a, kNull);
                const Cell& c = cells_[size_t(a)];
                set_ext(a, primary ? 0 : c.ext_primary, color ? 0 : c.ext_color);
            }
            addr = t + 1;
            break;
        }
        case 0x01: {  // SOH (FR 15.6.9): clears the format table first
            need(1);
            const uint8_t len = d[i++];
            if (len == 0 || len > 7) throw Error{kSenseSohLength};
            need(len);
            const uint8_t* h = &d[i];
            i += len;
            fields_.clear();
            ic_ = -1;
            soh_flags_ = h[0];
            resequence_ = len >= 3 ? h[2] : 0;
            error_row_header_ = len >= 4 ? h[3] : 0;
            pf_no_data_ = len >= 7 ? (uint32_t(h[4]) << 16) | (uint32_t(h[5]) << 8) | h[6] : 0;
            break;
        }
        case 0x10: {  // TD (RFC 1205 5.3): length excludes the length bytes
            need(2);
            const size_t len = (size_t(d[i]) << 8) | d[i + 1];
            i += 2;
            need(len);
            if (addr < 0 || addr + int(len) > size()) throw Error{kSenseWritePastEnd};
            for (size_t k = 0; k < len; ++k) set_ch(addr++, d[i + k]);
            i += len;
            break;
        }
        case 0x12: {  // WEA (FR 15.6.11); does not move the address
            need(2);
            const uint8_t type = d[i], v = d[i + 1];
            i += 2;
            if (addr < 0 || addr >= size()) throw Error{kSenseWritePastEnd};
            const Cell& c = cells_[size_t(addr)];
            if (type == 0x01) {
                if (v != 0 && (v < 0x80 || v > 0x9F)) throw Error{kSenseExtAttrType};
                set_ext(addr, v, c.ext_color);
            } else if (type == 0x03) {
                if (v != 0 && v < 0x80) throw Error{kSenseExtAttrType};
                set_ext(addr, c.ext_primary, v);
            } else {
                throw Error{kSenseExtAttrType};
            }
            break;
        }
        case 0x1D: {  // SF (FR 15.6.12)
            if (after_sf) addr = sf_next;
            need(1);
            uint16_t ffw = 0;
            bool input = false;
            std::vector<uint16_t> fcws;
            if ((d[i] & 0xC0) == 0x40) {
                need(2);
                ffw = uint16_t((d[i] << 8) | d[i + 1]);
                i += 2;
                input = true;
                while (i < d.size() && (d[i] & 0xC0) == 0x80) {
                    need(2);
                    fcws.push_back(uint16_t((d[i] << 8) | d[i + 1]));
                    i += 2;
                }
            }
            need(3);
            const uint8_t attr = d[i];
            const int len = (d[i + 1] << 8) | d[i + 2];
            i += 3;
            if ((attr & 0xE0) != 0x20) throw Error{kSenseFieldAttr};
            const int start = addr + 1;
            if (start < 0 || start >= size()) throw Error{kSenseFieldAddress};
            if (input && len < 1) throw Error{kSenseFieldLength};
            if (start + len > size()) throw Error{kSenseFieldPastEnd};
            if (addr >= 0) set_ch(addr, attr);
            if (input) {
                lock_keyboard();
                insert_ = false;
                pending_aid_ = 0;
                auto it = std::find_if(fields_.begin(), fields_.end(),
                                       [&](const Field& f) { return f.start == start; });
                if (it != fields_.end()) {
                    it->ffw = ffw;   // FCWs of a modified entry are ignored (15.6.12.2)
                    it->attr = attr;
                    it->length = len;
                } else {
                    // Drop anything the new field overlaps, then insert in order.
                    fields_.erase(std::remove_if(fields_.begin(), fields_.end(),
                                                 [&](const Field& f) {
                                                     return f.start - 1 <= start + len && f.end() >= start - 1;
                                                 }),
                                  fields_.end());
                    if (fields_.size() >= 256) throw Error{kSenseFormatOverflow};
                    Field f;
                    f.start = start;
                    f.length = len;
                    f.ffw = ffw;
                    f.fcws = std::move(fcws);
                    f.attr = attr;
                    auto pos = std::upper_bound(fields_.begin(), fields_.end(), start,
                                                [](int s, const Field& x) { return s < x.start; });
                    fields_.insert(pos, std::move(f));
                    if (start + len < size()) set_ch(start + len, 0x20);  // ending attribute
                }
            }
            addr = start;
            sf_follow = true;
            sf_next = start + len;
            break;
        }
        case 0x15: {  // WDSF (FR 15.6.13): GUI constructs, not claimed; skip
            need(2);
            const size_t ll = (size_t(d[i]) << 8) | d[i + 1];
            if (ll < 2) throw Error{kSenseSfLength};
            need(ll);
            TN_LOG("tn5250", Info, "WDSF of %zu bytes ignored", ll);
            i += ll;
            break;
        }
        default:  // data: characters, attributes and the low controls FR 15.6.2 allows
            if (b == 0xFF) throw Error{0x10050142};
            if (addr < 0 || addr >= size()) throw Error{kSenseWritePastEnd};
            set_ch(addr++, b);
            break;
        }
    }
    apply_cc1(cc1, mc);
}

void Engine::cmd_write_error_code(const std::vector<uint8_t>& d, size_t& i, bool window) {
    int sc = 0, ec = cols_ - 1;
    if (window) {  // FR 15.9
        if (i + 2 > d.size()) throw Error{kSensePrematureEnd};
        sc = d[i] - 1;
        ec = d[i + 1] - 1;
        i += 2;
        if (sc < 0 || ec <= sc || ec >= cols_) throw Error{kSenseWecwRowCol};
    }
    const int row = error_row();
    if (error_ == ErrorState::None) {
        saved_error_line_.assign(cells_.begin() + row * cols_, cells_.begin() + (row + 1) * cols_);
        locked_before_error_ = locked_;
        unlock_after_reset_ = false;
    }
    error_ = ErrorState::PreHelp;
    error_code_ = 0;
    insert_ = false;
    pending_aid_ = 0;
    for (int c = sc; c <= ec; ++c) set_ch(row * cols_ + c, kNull);
    int col = sc;
    bool any = false;
    while (i < d.size() && d[i] != 0x04) {
        if (d[i] == 0x13) {  // IC: moves the cursor only (FR 15.8)
            if (i + 3 > d.size()) throw Error{kSensePrematureEnd};
            const uint8_t r = d[i + 1], c = d[i + 2];
            if (r < 1 || r > rows_ || c < 1 || c > cols_) throw Error{kSenseRowCol};
            set_cursor((r - 1) * cols_ + (c - 1));
            i += 3;
            any = true;
            continue;
        }
        const uint8_t b = d[i++];
        if (col <= ec) set_ch(row * cols_ + col++, b);
        any = true;
    }
    if (!any) throw Error{kSensePrematureEnd};
    changed();
}

void Engine::cmd_read(uint8_t cmd, uint8_t cc0, uint8_t cc1) {
    apply_cc0(cc0);
    read_cmd_ = cmd;
    if (pending_aid_) {
        // An AID was held while no read was outstanding (FR 16.2.1).
        send(kOpPutGet, 0, build_read_reply(cmd, pending_aid_, false));
        pending_aid_ = 0;
        read_cmd_ = 0;
        return;
    }
    apply_cc1(cc1, -1);
}

void Engine::cmd_roll(uint8_t b1, uint8_t top, uint8_t bottom) {
    const int n = b1 & 0x1F;
    const bool down = (b1 & 0x80) != 0;
    if (top == 0 || bottom > rows_ || top >= bottom || n > bottom - top) throw Error{kSenseRollParam};
    if (n == 0) return;
    const int t = top - 1, btm = bottom - 1;
    std::vector<Cell> old(cells_.begin() + t * cols_, cells_.begin() + (btm + 1) * cols_);
    // Rows move by n; vacated rows keep their old contents (FR 15.25).
    for (int r = t; r <= btm; ++r) {
        const int src = down ? r - n : r + n;
        if (src < t || src > btm) continue;
        for (int c = 0; c < cols_; ++c) {
            const Cell& s = old[size_t((src - t) * cols_ + c)];
            set_ch(r * cols_ + c, s.ch);
            set_ext(r * cols_ + c, s.ext_primary, s.ext_color);
        }
    }
}

void Engine::cmd_write_structured_field(const std::vector<uint8_t>& d, size_t& i) {
    while (i < d.size() && d[i] != 0x04) {
        if (i + 4 > d.size()) throw Error{kSensePrematureEnd};
        const size_t ll = (size_t(d[i]) << 8) | d[i + 1];
        if (ll < 4) throw Error{kSenseSfLength};
        if (i + ll > d.size()) throw Error{kSensePrematureEnd};
        const uint8_t cls = d[i + 2], type = d[i + 3];
        if (cls == 0xD9 && type == 0x70) {
            send(kOpNoOp, 0, query_reply());
        } else {
            TN_LOG("tn5250", Info, "structured field class %02X type %02X ignored", cls, type);
        }
        i += ll;
    }
}

std::vector<uint8_t> Engine::query_reply() const {
    // FR 15.27.2 Tables 88 and 89, preceded by cursor 0000 and AID 88 (FR
    // 15.27, RFC 1205 section 5). The controller class 0600 is RFC 1205's
    // "other WSF or any other 5250 emulator", which is what we are.
    std::vector<uint8_t> r{0x00, 0x00, 0x88};
    put16(r, 0x0044);
    r.insert(r.end(), {0xD9, 0x70, 0x80});
    put16(r, 0x0600);                                   // controller hardware class
    r.insert(r.end(), {0x01, 0x03, 0x00});              // code level
    r.insert(r.end(), 16, 0x00);                        // reserved
    r.push_back(0x01);                                  // display station
    for (size_t k = 0; k < 4; ++k) r.push_back(ebcdic_invariant(k < opt_.machine_type.size() ? opt_.machine_type[k] : ' '));
    for (size_t k = 0; k < 3; ++k) r.push_back(ebcdic_invariant(k < opt_.model.size() ? opt_.model[k] : ' '));
    r.push_back(opt_.keyboard_id);
    r.push_back(0x00);                                  // extended keyboard ID
    r.push_back(0x00);                                  // reserved
    r.insert(r.end(), 4, 0x00);                         // serial number
    put16(r, 0x0100);                                   // 256 input fields
    r.push_back(0x00);                                  // no WSC customization
    r.insert(r.end(), 2, 0x00);                         // reserved
    // Device capabilities, 22 bytes. Byte 0: Read MDT Alternate (0x20),
    // PA1/PA2 (0x10), PA3 (0x08), MC/TD/transparent FCW (0x02), Read
    // Modified Immediate Alternate (0x01). Byte 1: 27x132 (0x20) only when
    // the device has it, 24x80 (0x10) always, colour (0x01) on the FC.
    // Nothing else is claimed: no extended attributes, GUI, DBCS or
    // graphics.
    r.push_back(0x3B);
    r.push_back(uint8_t(0x10 | (opt_.wide ? 0x20 : 0) | (opt_.color ? 0x01 : 0)));
    r.insert(r.end(), 20, 0x00);
    return r;
}

// ---------------------------------------------------------------------------
// Inbound data (FR 15.10-15.20)

bool Engine::aid_includes_data(uint8_t aid) const {
    switch (aid) {
    case 0xF1: case 0xF4: case 0xF5: case 0xD9: case 0xDA: case 0x3F: case 0x50: return true;
    default: break;
    }
    int pf = 0;
    if (aid >= 0x31 && aid <= 0x3C) pf = aid - 0x30;
    else if (aid >= 0xB1 && aid <= 0xBC) pf = aid - 0xB0 + 12;
    if (pf == 0) return false;
    return (pf_no_data_ & (1u << (pf - 1))) == 0;
}

std::vector<int> Engine::field_order() const {
    std::vector<int> out;
    const int n = int(fields_.size());
    if (resequence_ == 0 || resequence_ > n) {
        for (int k = 0; k < n; ++k) out.push_back(k);
        return out;
    }
    // Resequencing chain (FR 15.6.12.2): start at the SOH's field number,
    // follow 80nn FCWs (missing = next field), stop at 80FF or a loop.
    std::vector<bool> seen(size_t(n), false);
    int k = resequence_;
    while (k >= 1 && k <= n && !seen[size_t(k - 1)]) {
        seen[size_t(k - 1)] = true;
        out.push_back(k - 1);
        const int nx = fields_[size_t(k - 1)].fcw(0x80);
        if (nx == 0xFF) break;
        k = nx >= 0 ? nx : k + 1;
    }
    return out;
}

std::vector<uint8_t> Engine::field_data(const Field& f, bool keep_nulls, bool pad) const {
    std::vector<uint8_t> v;
    if (f.transparent()) {
        for (int a = f.start; a < f.end(); ++a) v.push_back(cells_[size_t(a)].ch);
        return v;
    }
    const bool sn = f.signed_numeric() && f.length >= 2;
    const int dn = sn ? f.length - 1 : f.length;
    for (int a = f.start; a < f.start + dn; ++a) v.push_back(cells_[size_t(a)].ch);
    if (sn && cells_[size_t(f.end() - 1)].ch == kMinus) {
        // Negative: zone D on the low-order digit; the sign itself is never sent.
        uint8_t& last = v.back();
        if (is_digit(last)) last = uint8_t(0xD0 | (last & 0x0F));
    }
    if (!pad)
        while (!v.empty() && v.back() == kNull) v.pop_back();
    if (!keep_nulls)
        for (uint8_t& b : v)
            if (b == kNull) b = kBlank;
    return v;
}

std::vector<uint8_t> Engine::build_read_reply(uint8_t cmd, uint8_t aid, bool immediate) const {
    const int cur = immediate ? cursor_ : (pending_aid_ ? pending_aid_cursor_ : cursor_);
    std::vector<uint8_t> out{uint8_t(cur / cols_ + 1), uint8_t(cur % cols_ + 1), aid};
    if (!immediate && !aid_includes_data(aid)) return out;
    const auto order = field_order();
    if (cmd == 0x42 || cmd == 0x72) {
        if (cmd == 0x42) {
            bool any = false;
            for (const Field& f : fields_) any = any || f.mdt();
            if (!any) return out;
        }
        for (int k : order) {
            const auto v = field_data(fields_[size_t(k)], false, true);
            out.insert(out.end(), v.begin(), v.end());
        }
        return out;
    }
    // 52 Read MDT, 82 Read MDT Alternate, 83 Read Modified Immediate Alternate.
    const bool keep_nulls = cmd != 0x52;
    for (int k : order) {
        const Field& f = fields_[size_t(k)];
        if (!f.mdt()) continue;
        out.push_back(0x11);
        out.push_back(uint8_t(f.start / cols_ + 1));
        out.push_back(uint8_t(f.start % cols_ + 1));
        const auto v = field_data(f, keep_nulls, false);
        if (f.transparent()) {
            out.push_back(0x10);
            put16(out, unsigned(v.size()));
        }
        out.insert(out.end(), v.begin(), v.end());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Save / restore image. Our own format; the host stores it opaquely.
//   'T' '3' '4' 'S' version length(2, whole image)
//   rows cols(2) cursor(2) ic(2, FFFF none)
//   locked insert blink error error_code(2) locked_before_error unlock_after_reset
//   soh_flags resequence error_row pf_no_data(3) read_cmd pending_aid aid_cursor(2)
//   field count(2) { start(2) length(2) ffw(2) attr fcw_count fcw(2)... }
//   RLE(ch plane) RLE(ext primary) RLE(ext colour)
//   saved error line: count(2), then RLE(ch) RLE(primary) RLE(colour)

std::vector<uint8_t> Engine::save_image() const {
    std::vector<uint8_t> v{'T', '3', '4', 'S', kImageVersion, 0, 0};
    v.push_back(uint8_t(rows_));
    put16(v, unsigned(cols_));
    put16(v, unsigned(cursor_));
    put16(v, ic_ < 0 ? 0xFFFFu : unsigned(ic_));
    v.push_back(locked_);
    v.push_back(insert_);
    v.push_back(blink_);
    v.push_back(uint8_t(error_));
    put16(v, error_code_);
    v.push_back(locked_before_error_);
    v.push_back(unlock_after_reset_);
    v.push_back(soh_flags_);
    v.push_back(resequence_);
    v.push_back(uint8_t(error_row_header_));
    v.push_back(uint8_t(pf_no_data_ >> 16));
    v.push_back(uint8_t(pf_no_data_ >> 8));
    v.push_back(uint8_t(pf_no_data_));
    v.push_back(read_cmd_);
    v.push_back(pending_aid_);
    put16(v, unsigned(pending_aid_cursor_));
    put16(v, unsigned(fields_.size()));
    for (const Field& f : fields_) {
        put16(v, unsigned(f.start));
        put16(v, unsigned(f.length));
        put16(v, f.ffw);
        v.push_back(f.attr);
        v.push_back(uint8_t(f.fcws.size()));
        for (uint16_t w : f.fcws) put16(v, w);
    }
    auto planes = [&](const std::vector<Cell>& cells) {
        std::vector<uint8_t> a, b, c;
        for (const Cell& x : cells) {
            a.push_back(x.ch);
            b.push_back(x.ext_primary);
            c.push_back(x.ext_color);
        }
        rle_encode(v, a);
        rle_encode(v, b);
        rle_encode(v, c);
    };
    planes(cells_);
    put16(v, unsigned(saved_error_line_.size()));
    planes(saved_error_line_);
    v[5] = uint8_t(v.size() >> 8);
    v[6] = uint8_t(v.size());
    return v;
}

bool Engine::restore_image(const uint8_t* p, size_t n, size_t& used) {
    if (n < 7 || p[0] != 'T' || p[1] != '3' || p[2] != '4' || p[3] != 'S' || p[4] != kImageVersion) return false;
    const size_t len = (size_t(p[5]) << 8) | p[6];
    if (len > n || len < 7) return false;
    Reader rd(p + 7, len - 7);
    const int r = rd.u8();
    const int c = int(rd.u16());
    if (!((r == 24 && c == 80) || (r == 27 && c == 132 && opt_.wide))) return false;
    const int sz = r * c;
    const int cursor = int(rd.u16());
    const unsigned ic = rd.u16();
    const bool locked = rd.u8() != 0;
    const bool insert = rd.u8() != 0;
    const bool blink = rd.u8() != 0;
    const uint8_t err = rd.u8();
    const uint16_t err_code = uint16_t(rd.u16());
    const bool lbe = rd.u8() != 0;
    const bool uar = rd.u8() != 0;
    const uint8_t sohf = rd.u8();
    const uint8_t reseq = rd.u8();
    const int erow = rd.u8();
    uint32_t pf = uint32_t(rd.u8()) << 16;
    pf |= uint32_t(rd.u8()) << 8;
    pf |= rd.u8();
    const uint8_t rcmd = rd.u8();
    const uint8_t paid = rd.u8();
    const int pcur = int(rd.u16());
    const unsigned nf = rd.u16();
    if (!rd.ok() || nf > 256 || cursor >= sz || pcur >= sz || err > 2) return false;
    std::vector<Field> fields;
    for (unsigned k = 0; k < nf; ++k) {
        Field f;
        f.start = int(rd.u16());
        f.length = int(rd.u16());
        f.ffw = uint16_t(rd.u16());
        f.attr = rd.u8();
        const unsigned nfcw = rd.u8();
        for (unsigned w = 0; w < nfcw; ++w) f.fcws.push_back(uint16_t(rd.u16()));
        if (!rd.ok() || f.start + f.length > sz) return false;
        fields.push_back(std::move(f));
    }
    auto planes = [&](std::vector<Cell>& out, size_t count) {
        std::vector<uint8_t> a, b, cc;
        if (!rd.rle(a, count) || !rd.rle(b, count) || !rd.rle(cc, count)) return false;
        out.assign(count, Cell{});
        for (size_t k = 0; k < count; ++k) {
            out[k].ch = a[k];
            out[k].ext_primary = b[k];
            out[k].ext_color = cc[k];
        }
        return true;
    };
    std::vector<Cell> cells, saved;
    if (!planes(cells, size_t(sz))) return false;
    const unsigned ns = rd.u16();
    if (!rd.ok() || (ns != 0 && ns != unsigned(c))) return false;
    if (ns && !planes(saved, ns)) return false;
    if (!rd.ok()) return false;

    rows_ = r;
    cols_ = c;
    cells_ = std::move(cells);
    cursor_ = cursor;
    ic_ = ic == 0xFFFF ? -1 : int(ic);
    locked_ = locked;
    insert_ = insert;
    blink_ = blink;
    error_ = ErrorState(err);
    error_code_ = err_code;
    locked_before_error_ = lbe;
    unlock_after_reset_ = uar;
    soh_flags_ = sohf;
    resequence_ = reseq;
    error_row_header_ = erow;
    pf_no_data_ = pf;
    read_cmd_ = rcmd;
    pending_aid_ = paid;
    pending_aid_cursor_ = pcur;
    fields_ = std::move(fields);
    saved_error_line_ = std::move(saved);
    fer_wait_ = false;
    for (Cell& x : cells_) x.attr = 0;  // force refresh_attrs to recompute
    dirty_ = Rect{};
    dirty_.add(0, 0);
    dirty_.add(rows_ - 1, cols_ - 1);
    changed();
    used = len;
    return true;
}

// ---------------------------------------------------------------------------
// Operator input (FR chapter 16)

int Engine::field_index_at(int addr) const {
    for (size_t k = 0; k < fields_.size(); ++k)
        if (addr >= fields_[k].start && addr < fields_[k].end()) return int(k);
    return -1;
}

int Engine::home_address() const {
    if (ic_ >= 0) return ic_;
    for (const Field& f : fields_)
        if (!f.bypass()) return f.start;
    return 0;
}

int Engine::next_input_field(int from, bool forward) const {
    const int n = int(fields_.size());
    if (forward) {
        for (int k = 0; k < n; ++k)
            if (!fields_[size_t(k)].bypass() && fields_[size_t(k)].start > from) return k;
        for (int k = 0; k < n; ++k)
            if (!fields_[size_t(k)].bypass()) return k;
    } else {
        for (int k = n - 1; k >= 0; --k)
            if (!fields_[size_t(k)].bypass() && fields_[size_t(k)].start < from) return k;
        for (int k = n - 1; k >= 0; --k)
            if (!fields_[size_t(k)].bypass()) return k;
    }
    return -1;
}

void Engine::lock_keyboard() {
    if (!locked_) {
        locked_ = true;
        changed();
    }
    insert_ = false;
}

KeyResult Engine::post_error(uint16_t code) {
    const int row = error_row();
    if (error_ == ErrorState::None) {
        saved_error_line_.assign(cells_.begin() + row * cols_, cells_.begin() + (row + 1) * cols_);
        locked_before_error_ = locked_;
        unlock_after_reset_ = false;
    }
    error_ = ErrorState::PreHelp;
    error_code_ = code;
    insert_ = false;
    ++alarms_;
    // Show the code on the error line: reverse-image attribute, four
    // digits (columns 2-5, which Help sends back), normal attribute.
    for (int c = 0; c < cols_; ++c) set_ch(row * cols_ + c, kNull);
    set_ch(row * cols_, 0x21);
    for (int k = 0; k < 4; ++k) set_ch(row * cols_ + 1 + k, uint8_t(0xF0 | ((code >> (12 - 4 * k)) & 0x0F)));
    set_ch(row * cols_ + 5, 0x20);
    refresh_attrs();
    changed();
    return KeyResult::Error;
}

void Engine::restore_error_line() {
    const int row = error_row();
    if (int(saved_error_line_.size()) == cols_)
        for (int c = 0; c < cols_; ++c) {
            const Cell& s = saved_error_line_[size_t(c)];
            set_ch(row * cols_ + c, s.ch);
            set_ext(row * cols_ + c, s.ext_primary, s.ext_color);
        }
    saved_error_line_.clear();
    error_ = ErrorState::None;
    error_code_ = 0;
    locked_ = locked_before_error_;
    if (unlock_after_reset_) {
        // The deferred CC1 unlock (FR Table 44 note 3). The cursor stays
        // where the error left it, typically on the field in error [design].
        locked_ = false;
        pending_aid_ = 0;
    }
    unlock_after_reset_ = false;
    refresh_attrs();
    changed();
}

bool Engine::right_adjust(Field& f, uint8_t fill) {
    const int first = f.start, last = f.last_enterable();
    int end = last;
    while (end >= first && cells_[size_t(end)].ch == kNull) --end;
    const int shift = last - end;
    if (end < first) {
        for (int a = first; a <= last; ++a) set_ch(a, fill);
        return true;
    }
    if (shift > 0) {
        for (int a = last; a >= first + shift; --a) set_ch(a, cells_[size_t(a - shift)].ch);
        for (int a = first; a < first + shift; ++a) set_ch(a, fill);
    }
    // Leading nulls become the fill character too.
    for (int a = first; a <= last && cells_[size_t(a)].ch == kNull; ++a) set_ch(a, fill);
    return true;
}

void Engine::advance_after_field(int fi) {
    const Field& f = fields_[size_t(fi)];
    if (f.auto_enter()) {
        send_aid(Aid::Enter);
        return;
    }
    if (f.fcw(0x85) == 0x01) {  // forward edge trigger
        send_aid(Aid::ForwardEdgeTrigger);
        return;
    }
    // Cursor progression FCW 88nn, ignored when resequencing is on.
    const int cp = resequence_ == 0 ? f.fcw(0x88) : -1;
    if (cp >= 1 && cp <= int(fields_.size()) && !fields_[size_t(cp - 1)].bypass()) {
        set_cursor(fields_[size_t(cp - 1)].start);
        return;
    }
    const int nf = next_input_field(f.start, true);
    set_cursor(nf >= 0 ? fields_[size_t(nf)].start : home_address());
}

KeyResult Engine::key_data(uint8_t c) {
    if (error_ != ErrorState::None || locked_) return KeyResult::Ignored;
    const int fi = field_index_at(cursor_);
    if (fi < 0 || fields_[size_t(fi)].bypass()) return post_error(kErrProtected);
    Field& f = fields_[size_t(fi)];
    if (fer_wait_) return post_error(kErrExitKey);
    if (f.signed_numeric() && cursor_ > f.last_enterable()) return post_error(kErrSignPosition);
    uint8_t ch = f.monocase() ? to_upper(c) : c;
    switch (f.shift()) {
    case 1:  // alphabetic only
        if (!(is_lower(ch) || is_upper(ch) || ch == 0x6B || ch == 0x4B || ch == kMinus || ch == kBlank))
            return post_error(kErrAlphaOnly);
        break;
    case 3:  // numeric only
        if (!(is_digit(ch) || ch == 0x4E || ch == 0x6B || ch == 0x4B || ch == kMinus || ch == kBlank))
            return post_error(kErrNumericOnly);
        break;
    case 5:  // digits only
    case 7:  // signed numeric
        if (!is_digit(ch)) return post_error(kErrDigitsOnly);
        break;
    case 6:  // I/O: only MSR or light pen may enter data
        return post_error(kErrProtected);
    default: break;
    }
    const int last = f.last_enterable();
    if (insert_) {
        // Insert needs a free last position. FR says null; a blank is
        // accepted too, as screens often pad fields with blanks [design].
        const uint8_t tail = cells_[size_t(last)].ch;
        if (tail != kNull && tail != kBlank) return post_error(kErrInsertFull);
        for (int a = last; a > cursor_; --a) set_ch(a, cells_[size_t(a - 1)].ch);
    }
    set_ch(cursor_, ch);
    set_mdt(f);
    if (cursor_ < last) set_cursor(cursor_ + 1);
    else if (f.fer()) fer_wait_ = true;
    else advance_after_field(fi);
    refresh_attrs();
    changed();
    return KeyResult::Accepted;
}

KeyResult Engine::field_exit(Key how) {
    const int fi = field_index_at(cursor_);
    if (fi < 0 || fields_[size_t(fi)].bypass()) return post_error(kErrProtected);
    Field& f = fields_[size_t(fi)];
    const bool numeric = f.shift() == 3 || f.signed_numeric();
    if (how == Key::FieldMinus && !numeric) return post_error(kErrFieldMinus);
    const int last = f.last_enterable();
    const int null_from = fer_wait_ ? last + 1 : cursor_;
    if (f.mandatory_enter() && !f.mdt()) return post_error(kErrMandatoryEnter);
    if (f.adjust() == 7) {
        // Mandatory fill: after nulling, the field must be full or empty.
        int filled = 0;
        for (int a = f.start; a < null_from && a <= last; ++a)
            if (cells_[size_t(a)].ch != kNull) ++filled;
        if (filled != 0 && filled != last - f.start + 1) return post_error(kErrMandatoryFill);
    }
    if (how == Key::FieldMinus && f.shift() == 3) {
        // Field- on numeric-only: zone D on the low-order position, which
        // must be a digit (FR Table 48).
        int a = std::min(null_from, last + 1) - 1;
        while (a >= f.start && cells_[size_t(a)].ch == kNull) --a;
        if (a < f.start || !is_digit(cells_[size_t(a)].ch)) return post_error(kErrFieldMinus);
    }
    for (int a = null_from; a <= last; ++a) set_ch(a, kNull);
    set_mdt(f);
    const unsigned adj = f.adjust();
    if (adj == 5 || adj == 6 || f.signed_numeric())
        right_adjust(f, adj == 5 ? 0xF0 : adj == 6 ? kBlank : kNull);
    if (f.signed_numeric()) set_ch(f.end() - 1, how == Key::FieldMinus ? kMinus : kBlank);
    if (how == Key::FieldMinus && f.shift() == 3) {
        int a = last;
        while (a >= f.start && cells_[size_t(a)].ch == kNull) --a;
        if (a >= f.start) set_ch(a, uint8_t(0xD0 | (cells_[size_t(a)].ch & 0x0F)));
    }
    fer_wait_ = false;
    advance_after_field(fi);
    return KeyResult::Accepted;
}

KeyResult Engine::send_aid(Aid aid) {
    const uint8_t a = uint8_t(aid);
    if (read_cmd_) {
        send(kOpPutGet, 0, build_read_reply(read_cmd_, a, false));
        read_cmd_ = 0;
    } else {
        pending_aid_ = a;
        pending_aid_cursor_ = cursor_;
    }
    lock_keyboard();
    fer_wait_ = false;
    changed();
    return KeyResult::Accepted;
}

KeyResult Engine::key_aid(Aid aid) {
    if (aid == Aid::Help) return key_function(Key::Help);
    if (aid == Aid::None || aid == Aid::QueryReply) return KeyResult::Ignored;
    if (error_ != ErrorState::None || locked_) return KeyResult::Ignored;
    const int fi = field_index_at(cursor_);
    if (fi >= 0 && !fields_[size_t(fi)].bypass()) {
        Field& f = fields_[size_t(fi)];
        if (f.adjust() == 7 && f.mdt()) {
            int filled = 0;
            for (int a = f.start; a <= f.last_enterable(); ++a)
                if (cells_[size_t(a)].ch != kNull) ++filled;
            if (filled != 0 && filled != f.last_enterable() - f.start + 1) return post_error(kErrMandatoryFill);
        }
        // FR 16.2.1 makes AID keys invalid in an active right-adjust field;
        // we adjust the field instead, as Field Exit would [design].
        if ((f.adjust() == 5 || f.adjust() == 6) && f.mdt()) right_adjust(f, f.adjust() == 5 ? 0xF0 : kBlank);
    }
    if (aid_includes_data(uint8_t(aid))) {
        bool master = false;
        for (const Field& f : fields_) master = master || f.mdt();
        if (master)
            for (const Field& f : fields_)
                if (!f.bypass() && f.mandatory_enter() && !f.mdt()) {
                    set_cursor(f.start);
                    return post_error(kErrMandatoryEnter);
                }
    }
    return send_aid(aid);
}

KeyResult Engine::key_function(Key key) {
    switch (key) {
    case Key::Attention:  // works locked or unlocked (FR 16.2.2)
        send(kOpNoOp, kFlagAtn, {});
        return KeyResult::Accepted;
    case Key::Reset:
        if (error_ != ErrorState::None) {
            restore_error_line();
            return KeyResult::Accepted;
        }
        if (insert_) {
            insert_ = false;
            changed();
            return KeyResult::Accepted;
        }
        return KeyResult::Ignored;
    case Key::Help:
        if (error_ == ErrorState::PreHelp) {
            // RFC 1205: HLP flag, error code as 4-digit packed decimal,
            // taken from error-line columns 2-5 dezoned (FR 15.8).
            const int base = error_row() * cols_;
            uint8_t nib[4];
            for (int k = 0; k < 4; ++k) nib[k] = uint8_t(cells_[size_t(base + 1 + k)].ch & 0x0F);
            send(kOpNoOp, kFlagHlp, {uint8_t(nib[0] << 4 | nib[1]), uint8_t(nib[2] << 4 | nib[3])});
            error_ = ErrorState::PostHelp;
            changed();
            return KeyResult::Accepted;
        }
        if (error_ != ErrorState::None || locked_) return KeyResult::Ignored;
        return send_aid(Aid::Help);
    case Key::SysReq:
        // RFC 1205 4.3: a header-only record with SRQ; the host answers
        // with Cancel Invite / Save Screen / its menu.
        if (error_ != ErrorState::None) return KeyResult::Ignored;
        send(kOpNoOp, kFlagSrq, {});
        return KeyResult::Accepted;
    default: break;
    }
    if (error_ != ErrorState::None || locked_) return KeyResult::Ignored;

    const bool keep_fer = key == Key::FieldExit || key == Key::FieldPlus || key == Key::FieldMinus ||
                          key == Key::Insert;
    if (!keep_fer) fer_wait_ = false;
    const int fi = field_index_at(cursor_);
    const bool in_input = fi >= 0 && !fields_[size_t(fi)].bypass();
    const int sz = size();
    switch (key) {
    case Key::CursorUp: set_cursor((cursor_ - cols_ + sz) % sz); break;
    case Key::CursorDown: set_cursor((cursor_ + cols_) % sz); break;
    case Key::CursorLeft:
    case Key::Backspace: set_cursor((cursor_ - 1 + sz) % sz); break;
    case Key::CursorRight: set_cursor((cursor_ + 1) % sz); break;
    case Key::Home: {
        const int h = home_address();
        if (cursor_ == h) return send_aid(Aid::RecordBackspace);  // FR 16.2.3
        set_cursor(h);
        break;
    }
    case Key::FieldAdvance: {
        const int n = next_input_field(cursor_, true);
        set_cursor(n >= 0 ? fields_[size_t(n)].start : 0);
        break;
    }
    case Key::FieldBackspace: {
        if (in_input && cursor_ > fields_[size_t(fi)].start) {
            set_cursor(fields_[size_t(fi)].start);
        } else {
            const int n = next_input_field(fi >= 0 ? fields_[size_t(fi)].start : cursor_, false);
            set_cursor(n >= 0 ? fields_[size_t(n)].start : 0);
        }
        break;
    }
    case Key::NewLine: {
        const int next_row = (cursor_ / cols_ + 1) * cols_;
        int target = -1;
        for (const Field& f : fields_)
            if (!f.bypass() && f.start >= next_row) { target = f.start; break; }
        if (target < 0) {
            const int n = next_input_field(-1, true);
            target = n >= 0 ? fields_[size_t(n)].start : next_row % sz;
        }
        set_cursor(target);
        break;
    }
    case Key::Insert:
        insert_ = !insert_;
        changed();
        break;
    case Key::Delete: {
        if (!in_input) return post_error(kErrProtected);
        Field& f = fields_[size_t(fi)];
        const int last = f.last_enterable();
        if (cursor_ > last) return post_error(kErrSignPosition);
        for (int a = cursor_; a < last; ++a) set_ch(a, cells_[size_t(a + 1)].ch);
        set_ch(last, kNull);
        set_mdt(f);
        break;
    }
    case Key::EraseEof: {
        if (!in_input) return post_error(kErrProtected);
        Field& f = fields_[size_t(fi)];
        for (int a = cursor_; a <= f.last_enterable(); ++a) set_ch(a, kNull);
        set_mdt(f);
        break;
    }
    case Key::EraseInput: {
        for (Field& f : fields_)
            if (!f.bypass() && f.mdt())
                for (int a = f.start; a <= f.last_enterable(); ++a) set_ch(a, kNull);
        const int n = next_input_field(-1, true);
        set_cursor(n >= 0 ? fields_[size_t(n)].start : home_address());
        break;
    }
    case Key::Dup: {
        if (!in_input) return post_error(kErrProtected);
        Field& f = fields_[size_t(fi)];
        if (!f.dup_enabled()) return post_error(kErrDupNotAllowed);
        for (int a = cursor_; a <= f.last_enterable(); ++a) set_ch(a, kDupChar);
        set_mdt(f);
        advance_after_field(fi);
        break;
    }
    case Key::FieldMark: {
        if (!in_input) return post_error(kErrProtected);
        Field& f = fields_[size_t(fi)];
        if (!f.dup_enabled()) return post_error(kErrDupNotAllowed);
        const int last = f.last_enterable();
        if (cursor_ > last) return post_error(kErrSignPosition);
        set_ch(cursor_, kFieldMarkChar);
        set_mdt(f);
        if (cursor_ < last) set_cursor(cursor_ + 1);
        else advance_after_field(fi);
        break;
    }
    case Key::FieldExit:
    case Key::FieldPlus:
    case Key::FieldMinus: {
        const KeyResult r = field_exit(key);
        refresh_attrs();
        return r;
    }
    default: return KeyResult::Ignored;
    }
    refresh_attrs();
    changed();
    return KeyResult::Accepted;
}

} // namespace tn5250
} // namespace em34xx
