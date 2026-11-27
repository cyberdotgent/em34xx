// SPDX-License-Identifier: GPL-3.0-or-later
// See cut_controller.hpp. The coax command codes and the CUT start-up
// sequence (poll, RESET, power-on-reset complete, terminal ID, feature IDs)
// are documented 3270 coax behaviour; the 3483's answers to them are
// observed in its firmware (analysis notes: the adapter specification).
#include "host/coax/cut_controller.hpp"

#include "tn3270/display_code.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

namespace em34xx {

namespace {

// Coax commands [documented].
constexpr uint8_t kPoll = 0x01, kPollAck = 0x11, kReset = 0x02, kReadTerminalId = 0x09, kLoadControl = 0x0A,
                  kLacHi = 0x04, kLacLo = 0x14, kWriteData = 0x0C;
// Feature commands: the feature address in the high nibble [documented].
constexpr uint8_t kReadFeatureId = 0x07, kEabWriteAlternate = 0x0A;
constexpr uint8_t kEabFeatureId = 0x79;   // EAB with colour [documented; observed: the 3483 answers 79]

constexpr uint64_t kPollIntervalNs = 4'000'000;   // 4 ms
constexpr uint64_t kProbeSettleNs = 2'000'000'000;   // 2 s of answers before RESET
constexpr unsigned kLostAfter = 8;                 // unanswered transmissions before starting over
constexpr int kStatusCells = 80;

// Terminal ID bits 3-1: the screen model [documented; observed: the 3483
// reports 04/06/0E/0C for models 2/3/4/5].
int rows_for_terminal_id(uint8_t id) {
    switch ((id >> 1) & 7) {
    case 3: return 32;
    case 7: return 43;
    case 6: return 27;
    default: return 24;
    }
}

CoaxWord cmd(uint8_t c) { return CoaxWord::command(c); }
CoaxWord data(uint8_t b) { return CoaxWord::data(b); }

// 3270 extended attribute values -> the EAB byte [documented: highlight
// bits 7-6, colour 5-3, character set 2-0].
uint8_t eab_for(uint8_t highlight, uint8_t colour, uint8_t charset) {
    uint8_t e = 0;
    if (highlight == 0xF1) e |= 0x40;        // blink
    else if (highlight == 0xF2) e |= 0x80;   // reverse
    else if (highlight == 0xF4) e |= 0xC0;   // underscore
    if (colour >= 0xF1 && colour <= 0xF7) e |= uint8_t((colour - 0xF0) << 3);
    if (charset == 0xF1) e |= 0x01;          // APL / GE
    return e;
}

} // namespace

CutController::CutController(std::unique_ptr<tn3270::Client> session) : session_(std::move(session)) {
    keys_.set_shift_releases_lock(true);
}

CutController::~CutController() {
    if (session_) session_->stop();
}

void CutController::restart() {
    phase_ = Phase::Probe;
    probe_seen_ = false;
    out_.clear();
    waiting_ = false;
    in_flight_ = Sent::None;
    silent_ = 0;
    shown_.clear();
    shown_eab_.clear();
    painted_gen_ = ~0ull;
    shown_cursor_ = -1;
    keys_.reset();
    break_next_ = false;
}

void CutController::terminal_reset() { restart(); }

void CutController::start_identify() {
    phase_ = Phase::Identify;
    queue({cmd(kReadTerminalId)}, Sent::TerminalId);
    feature_addr_ = 2;
    queue({cmd(uint8_t(feature_addr_ << 4 | kReadFeatureId))}, Sent::FeatureId);
}

void CutController::advance(uint64_t emu_ns) {
    now_ = emu_ns;
    if (session_) session_->pump();
    if (phase_ == Phase::Running && out_.empty() && !waiting_) paint();
}

bool CutController::to_terminal(CoaxMessage& msg) {
    if (waiting_) return false;
    if (!out_.empty()) {
        msg = std::move(out_.front().msg);
        in_flight_ = out_.front().kind;
        out_.pop_front();
        waiting_ = true;
        return true;
    }
    if (now_ < next_poll_) return false;
    next_poll_ = now_ + kPollIntervalNs;
    msg = {cmd(kPoll)};
    in_flight_ = Sent::Poll;
    waiting_ = true;
    return true;
}

void CutController::no_response() {
    waiting_ = false;
    if (phase_ == Phase::Probe) probe_seen_ = false;   // gone again: wait afresh
    const Sent what = in_flight_;
    in_flight_ = Sent::None;
    if (what == Sent::FeatureId) {
        // No feature at this address: try the next one [documented: the
        // controller probes addresses 2-F].
        if (++feature_addr_ <= 0xF) queue({cmd(uint8_t(feature_addr_ << 4 | kReadFeatureId))}, Sent::FeatureId);
        else {
            phase_ = Phase::Running;
            queue({cmd(kLoadControl), data(0x00)});   // cursor shown, display on
        }
        return;
    }
    if (++silent_ >= kLostAfter && phase_ != Phase::Probe) {
        LOG_INFO("coax", "terminal stopped answering; starting over");
        restart();
    }
}

void CutController::from_terminal(const CoaxMessage& msg) {
    waiting_ = false;
    silent_ = 0;
    const Sent what = in_flight_;
    in_flight_ = Sent::None;
    if (msg.empty()) return;
    const CoaxWord w = msg.front();
    switch (what) {
    case Sent::Poll: {
        if (phase_ == Phase::Probe) {
            // Something answers: reset it once it has answered for a while.
            // A terminal that has just come on line is still starting up
            // (the 3472 configures its keyboard then), and a RESET in the
            // middle of that leaves its keyboard disabled [observed in
            // emulation: after a terminal restart under a running
            // controller]. A new terminal answers the same way, so there is
            // nothing else to tell them apart by.
            if (!probe_seen_) {
                probe_seen_ = true;
                probe_since_ = now_;
                return;
            }
            if (now_ - probe_since_ < kProbeSettleNs) return;
            phase_ = Phase::Resetting;
            queue({cmd(kReset)}, Sent::Reset);
            return;
        }
        if (w.is_turnaround()) return;
        const bool flag = (w.bits & 2) != 0;
        queue({cmd(kPollAck)}, Sent::PollAck);
        if (phase_ == Phase::Resetting) {
            // Power-on reset complete [documented; observed: the 3483 arms
            // 0102 after RESET].
            if (flag && w.byte() == 0x02) start_identify();
            return;
        }
        if (flag) keystroke(w.byte());
        return;
    }
    case Sent::TerminalId:
        if (!w.is_command()) rows_ = rows_for_terminal_id(w.byte());
        return;
    case Sent::FeatureId:
        if (feature_addr_ == 7 || w.byte() == kEabFeatureId) eab_ = eab_ || w.byte() == kEabFeatureId;
        if (++feature_addr_ <= 0xF) queue({cmd(uint8_t(feature_addr_ << 4 | kReadFeatureId))}, Sent::FeatureId);
        else {
            phase_ = Phase::Running;
            queue({cmd(kLoadControl), data(0x00)});
        }
        return;
    default: return;
    }
}

bool CutController::decode_key(uint8_t code, wsc::OperatorAction& out) {
    // The poll response carries the keyboard's set-3 bytes as the 3483
    // received them: Shift is 12, then F0 12 when it is let go; Caps Lock
    // 14, F0 14 [observed: poll responses with the firmware's key modes,
    // typing keys make-only]. KeyboardMap takes a break as bit 7 (the 5250
    // convention), so F0 + code becomes code | 80. Before this, F0 was read
    // as a key of its own and the following 12 as a second make, so Shift
    // stayed down and Caps Lock toggled twice.
    if (code == 0xF0) {
        break_next_ = true;
        return false;
    }
    if (code & 0x80 && code != 0xFF) {
        // Set-3 make codes above 7F (keypad and Windows-type keys) have no
        // place in the 7-bit table [open]; their breaks are dropped too.
        break_next_ = false;
        out = wsc::OperatorAction{};
        out.scan = code;
        return true;   // Unmapped
    }
    const uint8_t scan = break_next_ ? uint8_t(code | 0x80) : code;
    break_next_ = false;
    return keys_.translate(scan, out);
}

void CutController::keystroke(uint8_t code) {
    wsc::OperatorAction a;
    if (!decode_key(code, a)) return;
    LOG_DEBUG("coax", "key %02X -> %s", code, wsc::operator_action_name(a.kind));
    if (!session_) return;
    using K = wsc::OperatorAction::Kind;
    using F = wsc::KeyFunction;
    using tn3270::Key;
    switch (a.kind) {
    case K::Character: session_->type(a.ebcdic); break;
    case K::Function:
        switch (a.function) {
        case F::Tab: case F::FieldExit: case F::FieldPlus: case F::FieldMinus: session_->key(Key::Tab); break;
        case F::Backtab: session_->key(Key::BackTab); break;
        case F::NewLine: session_->key(Key::NewLine); break;
        case F::Backspace: case F::CursorLeft: session_->key(Key::Left); break;
        case F::CursorRight: session_->key(Key::Right); break;
        case F::CursorUp: session_->key(Key::Up); break;
        case F::CursorDown: session_->key(Key::Down); break;
        case F::Home: session_->key(Key::Home); break;
        case F::Insert: session_->key(Key::Insert); break;
        case F::Delete: session_->key(Key::Delete); break;
        case F::Dup: session_->key(Key::Dup); break;
        case F::EraseEof: session_->key(Key::EraseEOF); break;
        case F::EraseInput: session_->key(Key::EraseInput); break;
        case F::Reset: session_->key(Key::Reset); break;
        case F::FieldMark: session_->key(Key::FieldMark); break;
        default: break;
        }
        break;
    case K::Aid: {
        // 5250 AID values from the keyboard map -> 3270 [documented both]:
        // Enter, F1-F12 -> PF1-12, F13-F24 -> PF13-24, Clear.
        uint8_t aid = 0;
        if (a.aid == 0xF1) aid = uint8_t(tn3270::Aid::Enter);
        else if (a.aid >= 0x31 && a.aid <= 0x3C) aid = tn3270::pf_aid(a.aid - 0x30);
        else if (a.aid >= 0xB1 && a.aid <= 0xBC) aid = tn3270::pf_aid(a.aid - 0xB0 + 12);
        else if (a.aid == 0xBD) aid = uint8_t(tn3270::Aid::Clear);
        if (aid) session_->aid(aid);
        break;
    }
    case K::SysReq: session_->aid(uint8_t(tn3270::Aid::SysReq)); break;
    default: break;
    }
}

uint8_t CutController::display_byte(int addr) const {
    if (addr < kStatusCells) return addr < int(status_.size()) ? uint8_t(status_[size_t(addr)]) : 0;
    if (!session_) return 0;
    const tn3270::Engine& e = session_->engine();
    const int i = addr - kStatusCells;
    if (i >= e.size()) return 0;
    const tn3270::Cell& c = e.cell(i);
    return c.fa ? tn3270::display_field_attribute(c.ch) : tn3270::ebcdic_to_display(c.ch);
}

uint8_t CutController::eab_byte(int addr) const {
    if (addr < kStatusCells || !session_) return 0;
    const tn3270::Engine& e = session_->engine();
    const int i = addr - kStatusCells;
    if (i >= e.size()) return 0;
    const tn3270::Cell& c = e.cell(i);
    return eab_for(c.highlight, c.colour, c.charset);
}

void CutController::paint_status() {
    // The status line belongs to the controller [documented]. Drawn as
    // text in the display code: the connection state, X SYSTEM while the
    // keyboard is locked, the insert indicator.
    // TN3270, or TN3270E and the LU name the host assigned (the configured
    // device name when the host accepted it).
    std::string text = "TN3270";
    if (session_ && session_->tn3270e()) {
        text = "TN3270E";
        const std::string lu = session_->lu_name();
        if (!lu.empty()) text += " " + lu;
    }
    if (!session_) text = "COAX";
    else if (session_->state() != tn3270::ConnState::Ready) text += " " + std::string(tn3270::conn_state_name(session_->state()));
    std::string line(kStatusCells, ' ');
    for (size_t i = 0; i < text.size() && i < 20; ++i) line[i] = text[i];   // "TN3270E " + 8-char LU fits
    if (session_ && session_->engine().keyboard_locked()) {
        const std::string x = "X SYSTEM";
        line.replace(20, x.size(), x);
    }
    if (session_ && session_->engine().insert_mode()) line[40] = '^';
    status_.assign(kStatusCells, 0x10);
    for (int i = 0; i < kStatusCells; ++i) {
        uint8_t code = 0x10;
        tn3270::display_code_for(char32_t(uint8_t(line[size_t(i)])), code);
        status_[size_t(i)] = char(code);
    }
}

void CutController::paint() {
    const uint64_t gen = session_ ? session_->engine().generation() + uint64_t(session_->state()) * 1000003u : 0;
    if (gen == painted_gen_ && !shown_.empty()) return;
    painted_gen_ = gen;
    paint_status();
    const int size = kStatusCells + rows_ * cols_;
    if (int(shown_.size()) != size) {
        // First paint: the buffer holds whatever it held; write it all.
        shown_.assign(size_t(size), 0xFF);
        shown_eab_.assign(size_t(size), 0xFF);
    }
    // Runs of changed cells, each one Load Address Counter + a write.
    int a = 0;
    while (a < size) {
        if (display_byte(a) == shown_[size_t(a)] && (!eab_ || eab_byte(a) == shown_eab_[size_t(a)])) {
            ++a;
            continue;
        }
        int end = a;
        while (end < size && end - a < 240 &&
               (display_byte(end) != shown_[size_t(end)] || (eab_ && eab_byte(end) != shown_eab_[size_t(end)])))
            ++end;
        queue({cmd(kLacHi), data(uint8_t(a >> 8))});
        queue({cmd(kLacLo), data(uint8_t(a))});
        CoaxMessage w;
        w.push_back(eab_ ? cmd(uint8_t(0x70 | kEabWriteAlternate)) : cmd(kWriteData));
        for (int i = a; i < end; ++i) {
            const uint8_t ch = display_byte(i);
            w.push_back(data(ch));
            shown_[size_t(i)] = ch;
            if (eab_) {
                const uint8_t e = eab_byte(i);
                w.push_back(data(e));
                shown_eab_[size_t(i)] = e;
            }
        }
        queue(std::move(w));
        a = end;
    }
    // The cursor: a CUT terminal shows it at its address counter.
    const int cursor = kStatusCells + (session_ ? session_->engine().cursor() : 0);
    queue({cmd(kLacHi), data(uint8_t(cursor >> 8))});
    queue({cmd(kLacLo), data(uint8_t(cursor))});
    shown_cursor_ = cursor;
}

std::string CutController::describe() const {
    static const char* const kPhase[] = {"probing", "resetting", "identifying", "running"};
    std::string s = format("coax CUT controller: %s, %dx%d%s\n", kPhase[int(phase_)], rows_, cols_,
                           eab_ ? ", extended attributes" : "");
    if (session_)
        s += format("  %s session: %s%s%s\n", session_->tn3270e() ? "TN3270E" : "TN3270",
                    tn3270::conn_state_name(session_->state()),
                    session_->lu_name().empty() ? "" : (", LU " + session_->lu_name()).c_str(),
                    session_->last_error().empty() ? "" : (" (" + session_->last_error() + ")").c_str());
    return s;
}

std::vector<LineSessionStatus> CutController::session_status() const {
    if (!session_) return {};
    LineSessionStatus st;
    st.address = 0;
    st.state = tn3270::conn_state_name(session_->state());
    st.connected = session_->state() == tn3270::ConnState::Ready;
    st.error = session_->last_error();
    return {st};
}

} // namespace em34xx
