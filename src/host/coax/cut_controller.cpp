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
// The extended attribute buffer at feature address 7: 79 with colour
// [documented; observed: the 3483 answers 79], 78 without [hypothesis,
// strong: the 3472 answers 78 for its display type 1, 79 for type 2
// (docs/bringup-3472.md)].
constexpr uint8_t kEabFeatureId = 0x79, kEabMonoFeatureId = 0x78;

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

CutController::CutController(ClientFactory make, Tn5250Config cfg, SessionCycleOptions cycle)
    : make_(std::move(make)), cfg_(std::move(cfg)),
      cycle_(SessionCycle::Hooks{[this] { return phase(); }, [this] { open_session(); }, [this] { close_session(); },
                                 [this] { return session_ ? session_->last_error() : std::string(); }, "coax"},
             cycle) {
    keys_.set_shift_releases_lock(true);
    if (make_) {
        session_ = make_();
        cycle_.tick();   // connect now
    }
}

CutController::~CutController() {
    if (session_) session_->stop();
}

SessionPhase CutController::phase() const {
    if (!session_) return SessionPhase::Idle;
    switch (session_->state()) {
    case tn3270::ConnState::Idle: return SessionPhase::Idle;
    case tn3270::ConnState::Ready: return SessionPhase::Ready;
    case tn3270::ConnState::Closed:
    case tn3270::ConnState::Failed: return SessionPhase::Down;
    default: return SessionPhase::Connecting;
    }
}

// A used client cannot be restarted: every session gets a new one, with a
// new screen; the terminal's buffer is painted again from it.
void CutController::close_session() {
    if (session_) session_->stop();
    session_ = make_ ? make_() : nullptr;
    painted_gen_ = ~0ull;
    shown_cursor_ = -1;
}

void CutController::open_session() {
    if (!session_ || session_->state() != tn3270::ConnState::Idle) close_session();
    if (!session_) return;
    held_ = false;
    LOG_INFO("coax", "connecting to %s:%u%s, terminal type %s, device %s", cfg_.host.c_str(), cfg_.port,
             cfg_.tls ? " (TLS)" : "", cfg_.terminal_type.c_str(),
             cfg_.device_name.empty() ? "(host assigned)" : cfg_.device_name.c_str());
    std::string error;
    if (!session_->start(error)) LOG_ERROR("coax", "%s", error.c_str());
}

bool CutController::disconnect_station(unsigned address) {
    if (address != 0 || !make_) return false;
    cycle_.stop();
    held_ = true;
    return true;
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

void CutController::terminal_reset() {
    restart();
    held_ = false;
    if (make_) cycle_.terminal_reset();   // a new run of the terminal meets a new session
}

void CutController::on_line() {
    phase_ = Phase::Running;
    queue({cmd(kLoadControl), data(0x00)});   // cursor shown, display on
    if (make_) cycle_.terminal_on_line();
}

void CutController::start_identify() {
    phase_ = Phase::Identify;
    eab_ = eab_colour_ = false;   // another terminal may have come on line
    queue({cmd(kReadTerminalId)}, Sent::TerminalId);
    feature_addr_ = 2;
    queue({cmd(uint8_t(feature_addr_ << 4 | kReadFeatureId))}, Sent::FeatureId);
}

void CutController::advance(uint64_t emu_ns) {
    now_ = emu_ns;
    if (make_) cycle_.tick();
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
        else on_line();
        return;
    }
    if (++silent_ >= kLostAfter && phase_ != Phase::Probe) {
        LOG_INFO("coax", "terminal stopped answering; starting over");
        // The terminal left the line: the session it was using is over (at
        // the next tick), and it meets a new one when it comes back.
        if (make_) cycle_.terminal_off_line();
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
        if (!w.is_command()) {
            rows_ = rows_for_terminal_id(w.byte());
            LOG_INFO("coax", "terminal ID %02X (%d rows)", w.byte(), rows_);
        }
        return;
    case Sent::FeatureId:
        if (!w.is_command() && w.byte() != 0x00) LOG_INFO("coax", "feature ID %02X at address %X", w.byte(), feature_addr_);
        if (!w.is_command() && (w.byte() == kEabFeatureId || w.byte() == kEabMonoFeatureId)) {
            eab_ = true;
            eab_colour_ = w.byte() == kEabFeatureId;
        }
        if (++feature_addr_ <= 0xF) queue({cmd(uint8_t(feature_addr_ << 4 | kReadFeatureId))}, Sent::FeatureId);
        else on_line();
        return;
    default: return;
    }
}

namespace {

// The 3270 legends of the 122-key keyboard, by set-3 key position
// [documented: IBM Personal Communications/3270 V4.0 Reference Guide for
// Windows, S20H-1773, 5.1.1, "PS/2 122 Key Keyboard", whose segments A and C
// are the keyboard's left-hand and editing blocks; hypothesis: that the
// keycaps of the 3483's and 3472's 122-key keyboards say the same]. The
// positions follow the TMK set-3 map (docs/keyboard.md). Returns false for
// a position that is not a 3270 function key here (a typing key).
bool function_key_3270(uint8_t pos, bool shift, bool alt, CoaxKey& out) {
    using Kind = CoaxKey::Kind;
    using tn3270::Aid;
    using tn3270::Key;
    auto key = [&](Key k) { out.kind = Kind::Key; out.key = k; return true; };
    auto aid = [&](uint8_t a) { out.kind = Kind::Aid; out.aid = a; return true; };
    auto none = [&] { out.kind = Kind::None; return true; };
    // F1-F12 (the lower row) and F13-F24 (the upper row) -> PF1-PF24.
    static constexpr uint8_t kLower[12] = {0x07, 0x0F, 0x17, 0x1F, 0x27, 0x2F, 0x37, 0x3F, 0x47, 0x4F, 0x56, 0x5E};
    static constexpr uint8_t kUpper[12] = {0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x57, 0x5F};
    for (int i = 0; i < 12; ++i) {
        if (pos == kLower[i]) return aid(tn3270::pf_aid(i + 1));
        if (pos == kUpper[i]) return aid(tn3270::pf_aid(i + 13));
    }
    switch (pos) {
    // Left-hand block, top to bottom: Attn (SysReq with Alt; with Shift
    // too [ours: the toolbar's SysRq]) | Clear; CurSel | Erase Input;
    // (blank) | Erase EOF; (blank) | Play; (blank) | (blank).
    case 0x05:
        out.kind = (shift || alt) ? Kind::SysReq : Kind::Attn;
        return true;
    case 0x06: return aid(uint8_t(Aid::Clear));
    case 0x0C: return key(Key::EraseInput);
    case 0x0B: return alt ? none() : key(Key::EraseEOF);   // Alt: Record, local
    case 0x04: case 0x03: case 0x0A: case 0x83: case 0x01: case 0x09:
        return none();   // CurSel, Play, Setup: local or not 3270 keys
    // Editing block: PA1 (Shift: Dup), PA2 (Shift: Field Mark), Jump
    // (Alt: PA3) over BackTab, Insert, Delete.
    case 0x67: return shift ? key(Key::Dup) : aid(uint8_t(Aid::PA1));
    case 0x6E: return shift ? key(Key::FieldMark) : aid(uint8_t(Aid::PA2));
    case 0x6F: return (alt || shift) ? aid(uint8_t(Aid::PA3)) : none();
    case 0x64: return key(Key::BackTab);
    case 0x65: return key(Key::Insert);
    case 0x6D: return alt ? none() : key(Key::Delete);   // Alt: Delete Word
    // Cursor block: Rule (Alt: Home) between the arrows.
    case 0x62: return alt ? key(Key::Home) : none();   // Rule is local
    case 0x63: return key(Key::Up);
    case 0x60: return key(Key::Down);
    case 0x61: return key(Key::Left);
    case 0x6A: return key(Key::Right);
    // Typing block and keypad.
    case 0x66: return key(Key::Backspace);
    case 0x0D: return key(shift ? Key::BackTab : Key::Tab);
    case 0x5A: return key(Key::NewLine);
    case 0x58: case 0x79: return aid(uint8_t(Aid::Enter));   // Enter; keypad Enter
    case 0x7C: return key(Key::Tab);                          // keypad Tab Field
    case 0x11: return alt ? none() : key(Key::Reset);          // Alt: Quit
    case 0x53:   // \ | left of New Line [ours: US legends]
        out.kind = Kind::Character;
        out.ebcdic = shift ? 0x4F : 0xE0;
        return true;
    default: return false;
    }
}

} // namespace

bool CutController::decode_key(uint8_t code, CoaxKey& out) {
    out = CoaxKey{};
    // The poll response carries the keyboard's set-3 bytes as the 3483
    // received them: Shift is 12, then F0 12 when it is let go; Caps Lock
    // 14, F0 14 [observed: poll responses with the firmware's key modes,
    // typing keys make-only]. KeyboardMap takes a break as bit 7 (the 5250
    // convention), so F0 + code becomes code | 80.
    if (code == 0xF0) {
        break_next_ = true;
        return false;
    }
    out.position = code;
    if (code & 0x80 && code != 0xFF) {
        // Set-3 codes above 7F: 83 is a left-hand block key (above); the
        // others have no 3270 legend here [open]; breaks are dropped.
        const bool brk = break_next_;
        break_next_ = false;
        if (brk) return false;
        function_key_3270(code, keys_.shift(), keys_.alt(), out);
        return true;
    }
    const uint8_t scan = break_next_ ? uint8_t(code | 0x80) : code;
    break_next_ = false;
    // Shift, Alt and Caps Lock state, and the typing keys' characters.
    wsc::OperatorAction a;
    if (!keys_.translate(scan, a)) return false;
    if (a.kind == wsc::OperatorAction::Kind::Overrun) return true;
    if (function_key_3270(code, keys_.shift(), keys_.alt(), out)) return true;
    if (a.kind == wsc::OperatorAction::Kind::Character) {
        out.kind = CoaxKey::Kind::Character;
        out.ebcdic = a.ebcdic;
    }
    return true;
}

void CutController::keystroke(uint8_t code) {
    CoaxKey k;
    if (!decode_key(code, k)) return;
    using Kind = CoaxKey::Kind;
    static const char* const kKind[] = {"none", "character", "key", "aid", "attn", "sysreq"};
    LOG_DEBUG("coax", "key %02X -> %s", code, kKind[int(k.kind)]);
    if (!session_ || session_->state() != tn3270::ConnState::Ready) return;
    switch (k.kind) {
    case Kind::Character: session_->type(k.ebcdic); break;
    case Kind::Key: session_->key(k.key); break;
    case Kind::Aid: session_->aid(k.aid); break;
    case Kind::Attn: session_->attn(); break;
    case Kind::SysReq: session_->sysreq(); break;
    case Kind::None: break;
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
    // Highlighting and character set only on a monochrome buffer.
    return eab_for(c.highlight, eab_colour_ ? c.colour : 0, c.charset);
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
    if (!make_) text = "COAX";
    else if (held_) text += " disconnected";
    else if (session_ && session_->state() != tn3270::ConnState::Ready)
        text += " " + std::string(tn3270::conn_state_name(session_->state()));
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
                           eab_ ? (eab_colour_ ? ", extended attributes with colour" : ", extended attributes")
                                : "");
    if (session_)
        s += format("  %s session: %s%s%s\n", session_->tn3270e() ? "TN3270E" : "TN3270",
                    tn3270::conn_state_name(session_->state()),
                    session_->lu_name().empty() ? "" : (", LU " + session_->lu_name()).c_str(),
                    session_->last_error().empty() ? "" : (" (" + session_->last_error() + ")").c_str());
    return s;
}

std::vector<LineSessionStatus> CutController::session_status() const {
    if (!make_) return {};
    LineSessionStatus st;
    st.address = 0;
    st.held = held_;
    st.state = held_ ? "disconnected" : session_ ? tn3270::conn_state_name(session_->state()) : "idle";
    st.connected = !held_ && session_ && session_->state() == tn3270::ConnState::Ready;
    st.peer = format("%s:%u", cfg_.host.c_str(), cfg_.port);
    st.tls = cfg_.tls;
    st.error = session_ ? session_->last_error() : std::string();
    return {st};
}

} // namespace em34xx
