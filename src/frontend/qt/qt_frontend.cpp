// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/qt/qt_frontend.hpp"

#include "app/application.hpp"
#include "app/host_link.hpp"
#include "config/profile.hpp"
#include "em34xx_version.h"
#include "frontend/qt/qt_dialogs.hpp"
#include "frontend/qt/qt_keys.hpp"
#include "frontend/qt/qt_util.hpp"
#include "input/host_keymap.hpp"
#include "machine/emulator.hpp"
#include "models/model.hpp"
#include "present/display_profile.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QScreen>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>
#ifndef QT_NO_SESSIONMANAGER
#include <QSessionManager>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

// No moc: nothing here declares signals or slots (no Q_OBJECT); connections
// are to lambdas and events are handled by overriding the virtuals.

namespace em34xx {

using namespace qt;

namespace {

using steady = std::chrono::steady_clock;

// The machine may report a change after every execution slice; fetch the
// raster (which holds the machine lock) at most once per refresh interval.
constexpr int kMinRasterIntervalMs = 15;
// Quit requests and signals.
constexpr int kHousekeepingMs = 100;
// Session status, deferred trust prompts, the tube's wear.
constexpr int kStatusMs = 500;

// --- TLS trust requests: I/O threads wait, the UI thread answers ------------

class TrustQueue {
public:
    struct Pending {
        tn5250::TlsTrustRequest req;
        bool done = false;
        // ask() has returned without this answer (it waited too long, or the
        // queue closed): an answer given now reaches no session.
        bool abandoned = false;
        tn5250::TlsTrustDecision decision = tn5250::TlsTrustDecision::Reject;
    };

    // `notify` runs on the UI thread (queued to `context`) when a request
    // arrives.
    void attach(QObject* context, std::function<void()> notify) {
        std::lock_guard<std::mutex> g(mu_);
        context_ = context;
        notify_ = std::move(notify);
    }

    // On a session's I/O thread.
    tn5250::TlsTrustDecision ask(const tn5250::TlsTrustRequest& req) {
        std::unique_lock<std::mutex> lock(mu_);
        if (closed_ || draining_ || !context_) return tn5250::TlsTrustDecision::Reject;
        auto p = std::make_shared<Pending>();
        p->req = req;
        queue_.push_back(p);
        // Under the lock: close() clears the context before it goes.
        QMetaObject::invokeMethod(context_, notify_, Qt::QueuedConnection);
        // The server may drop an idle handshake; ten minutes is plenty.
        cv_.wait_for(lock, std::chrono::minutes(10), [&] { return p->done || closed_; });
        if (!p->done) {
            p->abandoned = true;
            queue_.erase(std::remove(queue_.begin(), queue_.end(), p), queue_.end());
            return tn5250::TlsTrustDecision::Reject;
        }
        return p->decision;
    }

    bool empty() {
        std::lock_guard<std::mutex> g(mu_);
        return queue_.empty();
    }

    std::shared_ptr<Pending> take() {
        std::lock_guard<std::mutex> g(mu_);
        if (queue_.empty()) return nullptr;
        auto p = queue_.front();
        queue_.pop_front();
        return p;
    }

    // False when the session no longer waits for it (Pending::abandoned):
    // then nothing may come of the answer.
    bool answer(const std::shared_ptr<Pending>& p, tn5250::TlsTrustDecision d) {
        std::lock_guard<std::mutex> g(mu_);
        if (p->abandoned) return false;
        p->decision = d;
        p->done = true;
        cv_.notify_all();
        return true;
    }

    // While sessions are torn down (TrustDrain): the UI thread joins their
    // threads, so everything still waiting is rejected and ask() rejects at
    // once; a session that reaches its certificate check meanwhile must not
    // wait for a prompt the UI thread cannot show. Nests.
    void begin_drain() {
        std::lock_guard<std::mutex> g(mu_);
        ++draining_;
        for (auto& p : queue_) p->done = true;
        queue_.clear();
        cv_.notify_all();
    }
    void end_drain() {
        std::lock_guard<std::mutex> g(mu_);
        if (draining_ > 0) --draining_;
    }

    void close() {
        std::lock_guard<std::mutex> g(mu_);
        closed_ = true;
        context_ = nullptr;
        for (auto& p : queue_) p->done = true;
        queue_.clear();
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Pending>> queue_;
    QObject* context_ = nullptr;
    std::function<void()> notify_;
    bool closed_ = false;
    int draining_ = 0;
};

// Around a teardown of sessions (disconnect, machine rebuild).
class TrustDrain {
public:
    explicit TrustDrain(TrustQueue& q) : q_(q) { q_.begin_drain(); }
    ~TrustDrain() { q_.end_drain(); }
    TrustDrain(const TrustDrain&) = delete;
    TrustDrain& operator=(const TrustDrain&) = delete;

private:
    TrustQueue& q_;
};

// The profile the window is connected with (or last was).
struct CurrentProfile {
    bool active = false;
    Profile p;
    std::string name;      // file stem; "" when untitled
    std::string path;
    bool untitled = false;
    bool modified = false;
    bool accepted_once = false;   // a certificate was accepted for this connection only
    // Per session: it runs on a certificate that is not valid (expired, not
    // yet valid, revoked), let through by the user for that connection.
    bool overridden[Profile::kSessions] = {};
};

const char kUntitled[] = "Untitled Profile";

// Per session in the status bar: an orb (its state) and a lock (TLS).
struct SessionIndicator {
    const char* orb = "orb-grey";
    const char* lock = "lock-open-dim";
    std::string tip;
    bool operator==(const SessionIndicator& o) const {
        return std::string(orb) == o.orb && std::string(lock) == o.lock && tip == o.tip;
    }
    bool operator!=(const SessionIndicator& o) const { return !(*this == o); }
};

// The emulation thread's way to the screen widget. The display callback
// runs on the emulation thread, and clearing it does not wait for a call
// that thread is in the middle of making; so the callback holds this, not
// the widget, and reaches the widget only under the mutex. Once the UI
// thread has set `screen` to null (before the window goes) nobody touches
// the widget again.
class ScreenWidget;
struct ScreenLink {
    std::mutex mutex;
    ScreenWidget* screen = nullptr;
    bool wake_pending = false;   // one queued call in flight at a time
};

// The terminal's screen: the tube image at the widget's real pixel size,
// centred on black, and the host keyboard while it has the focus.
class ScreenWidget : public QWidget {
public:
    // Ctrl+Shift chords that belong to the window's menus.
    std::function<bool(const QKeySequence&)> is_shortcut;

    ScreenWidget(Emulator& emu, const CrtParams& crt, QWidget* parent)
        : QWidget(parent), emu_(emu), crt_params_(crt) {
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMinimumSize(320, 240);
        raster_timer_.setSingleShot(true);
        QObject::connect(&raster_timer_, &QTimer::timeout, this, [this] { fetch(); });
        fetch_raster();
    }

    // The presented frame (aspect-corrected) in logical pixels.
    QSize presented_size() const {
        return QSize(std::max(1, fb_.presented_width()), std::max(1, fb_.presented_height()));
    }
    const Framebuffer& framebuffer() const { return fb_; }
    void set_size_hint(QSize s) {
        hint_ = s;
        updateGeometry();
    }
    QSize sizeHint() const override { return hint_.isValid() ? hint_ : presented_size(); }

    const CrtParams& crt() const { return crt_params_; }
    void set_crt(const CrtParams& p) {
        crt_params_ = p;
        redraw();
    }
    // The tube's wear to draw (null: burn-in off).
    void set_tube(const TubeWear* tube) {
        if (tube_ == tube) return;
        tube_ = tube;
        redraw();
    }
    // Everything again: a rebuilt machine, other CRT settings, new wear.
    void redraw() {
        crt_.invalidate();
        px_w_ = -1;
        picture_changed();
    }

    // The emulator reported a new picture (UI thread). Coalesces into one
    // fetch per refresh interval.
    void picture_changed() {
        if (raster_timer_.isActive()) return;
        const auto since =
            std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - last_raster_time_).count();
        raster_timer_.start(since >= kMinRasterIntervalMs ? 0 : int(kMinRasterIntervalMs - since));
    }
    // Fallback for a missed wake-up (a rebuilt machine, say).
    void poll_generation() {
        if (emu_.display_generation() != seen_generation_) picture_changed();
    }

    // Release every host key that is down: the window lost the focus, or a
    // menu or dialog is about to take the keyboard.
    void release_keys() {
        if (pressed_.empty()) return;
        const std::set<uint32_t> held = pressed_;
        pressed_.clear();
        emu_.with_machine([&](Machine& m) {
            for (uint32_t hid : held) {
                HostKeyEvent ev;
                ev.hid_usage = hid;
                ev.pressed = false;
                ev.name = host_key_name(hid);
                m.key_event(ev);
            }
        });
    }

protected:
    bool event(QEvent* e) override {
        switch (e->type()) {
        case QEvent::ShortcutOverride: {
            // Every key belongs to the terminal while the screen has the
            // focus (no menu mnemonics on Alt, no F10), except the window's
            // own Ctrl+Shift accelerators.
            auto* k = static_cast<QKeyEvent*>(e);
            if (is_shortcut && is_shortcut(QKeySequence(k->keyCombination()))) return QWidget::event(e);
            e->accept();
            return true;
        }
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            // Before QWidget::event: it would use Tab for focus navigation.
            key(static_cast<QKeyEvent*>(e));
            e->accept();
            return true;
        default:
            return QWidget::event(e);
        }
    }
    void focusOutEvent(QFocusEvent* e) override {
        release_keys();
        QWidget::focusOutEvent(e);
    }
    void paintEvent(QPaintEvent*) override {
        render_tube();   // no-op unless the pixel size changed (resize, new scale factor)
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (image_.empty() || out_w_ <= 0 || out_h_ <= 0) return;
        const qreal dpr = devicePixelRatioF();
        QImage img(reinterpret_cast<const uchar*>(image_.data()), out_w_, out_h_, qsizetype(out_w_) * 4,
                   QImage::Format_RGB32);
        img.setDevicePixelRatio(dpr);
        // 1:1 in device pixels, centred (letterboxed).
        p.drawImage(QPointF(qreal((px_w_ - out_w_) / 2) / dpr, qreal((px_h_ - out_h_) / 2) / dpr), img);
    }

private:
    void key(QKeyEvent* e) {
        const bool down = e->type() == QEvent::KeyPress;
        const uint32_t hid = hid_from_xkb_keycode(e->nativeScanCode());
        TN_LOG("input", Debug, "qt %s scan code %u -> HID %02X (%s) key %08X mod %08X%s", down ? "down" : "up  ",
               unsigned(e->nativeScanCode()), unsigned(hid), hid_key_name(hid), unsigned(e->key()),
               unsigned(e->modifiers()), e->isAutoRepeat() ? " REPEAT" : "");
        if (e->isAutoRepeat() || hid == 0) return;   // the terminal does its own typematic
        if (down) {
            if (!pressed_.insert(hid).second) return;
        } else if (pressed_.erase(hid) == 0) {
            return;   // pressed before the screen had the focus
        }
        HostKeyEvent ev;
        ev.hid_usage = hid;
        ev.pressed = down;
        ev.name = host_key_name(hid);
        emu_.with_machine([&](Machine& m) { m.key_event(ev); });
    }

    void fetch_raster() {
        seen_generation_ = emu_.display_generation();
        emu_.with_machine([&](Machine& m) { render_presented(m, fb_); });
        last_raster_time_ = steady::now();
    }
    void fetch() {
        fetch_raster();
        render_tube();
    }
    // The tube image at the widget's pixel size; schedules a repaint of the
    // rows that changed.
    void render_tube() {
        const qreal dpr = devicePixelRatioF();
        const int px_w = std::max(1, int(std::lround(width() * dpr)));
        const int px_h = std::max(1, int(std::lround(height() * dpr)));
        int out_w = 1, out_h = 1;
        crt_fit(fb_, px_w, px_h, out_w, out_h);
        const bool resized = px_w != px_w_ || px_h != px_h_ || out_w != out_w_ || out_h != out_h_;
        if (resized) {
            px_w_ = px_w;
            px_h_ = px_h;
            out_w_ = out_w;
            out_h_ = out_h;
            crt_.invalidate();
        }
        const CrtRenderer::Rows rows = crt_.render(fb_, out_w, out_h, crt_params_, image_, tube_);
        if (resized) {
            update();
        } else if (!rows.empty()) {
            const int y0 = (px_h - out_h) / 2;
            const int top = int(std::floor((y0 + rows.row0) / dpr)) - 1;
            const int bottom = int(std::ceil((y0 + rows.row1) / dpr)) + 1;
            update(0, top, width(), bottom - top);
        }
    }

    Emulator& emu_;
    CrtParams crt_params_;
    const TubeWear* tube_ = nullptr;
    Framebuffer fb_;
    CrtRenderer crt_;
    std::vector<uint32_t> image_;
    int px_w_ = -1, px_h_ = -1;   // widget size in device pixels the image was made for
    int out_w_ = 0, out_h_ = 0;   // image size
    QSize hint_;
    QTimer raster_timer_;
    steady::time_point last_raster_time_{};
    uint64_t seen_generation_ = 0;
    std::set<uint32_t> pressed_;   // HID usages down, as sent to the machine
};

// Text from a profile file, for a message box: no control characters (a
// host name with line breaks could dress the question up).
std::string printable(const std::string& text) {
    std::string out = text;
    for (char& c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = '?';
    return out;
}

// What opening a profile from outside the profiles folder would do: each
// session's host and port and how its certificate is checked (as
// profile_connect_config() applies the profile's TLS settings).
std::string foreign_profile_question(const std::string& path, const Profile& p) {
    std::string text = "This profile is not in your profiles folder:\n\n" + printable(path) +
                       "\n\nOpening it connects to:\n\n";
    for (size_t i = 0; i < Profile::kSessions; ++i) {
        const ProfileSession& ps = p.sessions[i];
        if (!ps.used()) continue;
        text += format("Session %u: %s:%u - ", unsigned(i + 1), printable(ps.host).c_str(), unsigned(ps.port));
        const std::string pin = !ps.cert_sha256.empty()                      ? ps.cert_sha256
                                : p.tls_mode == ProfileTlsMode::Fingerprint ? p.tls_cert_sha256
                                                                            : std::string();
        if (!ps.tls) text += "TLS off (plain telnet, not encrypted)";
        else if (p.tls_mode == ProfileTlsMode::AcceptAll) text += "TLS, certificate NOT VERIFIED";
        else if (!pin.empty()) text += "TLS, only the certificate with SHA-256 " + printable(pin);
        else text += "TLS, verified";
        text += "\n";
    }
    text += "\nA profile from someone else can connect you to a system that imitates your sign-on screen, and "
            "it carries its own certificate settings. Connect only when you know where this file comes from "
            "and recognise these systems.\n\nConnect?";
    return text;
}

std::string file_stem(const std::string& path) { return std::filesystem::path(path).stem().string(); }

bool settings_maximized(const AppSettings& s) { return s.window.valid && s.window.maximized; }

// --- SIGINT and SIGTERM ------------------------------------------------------
//
// Ctrl+C in the terminal em34xx-qt was started from, kill, or the end of
// the session: leave through the normal exit path, which saves the
// battery-backed RAM, as closing the window does. The handler only sets a
// flag; the housekeeping timer acts on it.

volatile std::sig_atomic_t g_signal = 0;

extern "C" void qt_frontend_signal(int sig) { g_signal = sig; }

class SignalScope {
public:
    SignalScope() {
        struct sigaction sa = {};
        sa.sa_handler = qt_frontend_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        ::sigaction(SIGINT, &sa, &old_int_);
        ::sigaction(SIGTERM, &sa, &old_term_);
    }
    ~SignalScope() {
        ::sigaction(SIGINT, &old_int_, nullptr);
        ::sigaction(SIGTERM, &old_term_, nullptr);
    }

private:
    struct sigaction old_int_ = {};
    struct sigaction old_term_ = {};
};

// ---------------------------------------------------------------------------
// The main window

class MainWindow : public QMainWindow {
public:
    MainWindow(Application& app, QtLaunch& launch, const FrontendOptions& opts, std::atomic<bool>& quit);
    ~MainWindow() override;

    void set_texts(const std::string& title, const std::string& note) {
        title_ = title;
        note_ = note;
        update_title();
    }
    // Once the window is up: the profile from the command line, the startup
    // profile, or the profile manager.
    void startup();
    // The end of the desktop session: ask about the profile; write what a
    // clean exit writes.
    bool query_end_session();
    void end_session();

protected:
    void closeEvent(QCloseEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    QAction* add_action(QMenu* menu, const QString& text, std::function<void()> fn,
                        const QKeySequence& shortcut = QKeySequence());
    void build_menus();
    void build_toolbar();
    void build_status_bar();
    void apply_zoom();
    void restore_placement();
    void save_placement();
    void save_settings_file();

    void terminal_key(const char* function);

    void update_title();
    void update_status();
    void update_ui_state();
    std::string profile_display_name() const;
    SessionIndicator session_indicator(unsigned session, const std::vector<TwinaxSessionStatus>& all) const;
    std::string tls_state_text(const TwinaxSessionStatus* s) const;

    bool apply_terminal_settings(const Profile& p);
    const Model* profile_model(const Profile& p, const std::string& what);
    enum class ModelSwitch { Same, Switched, Failed };
    ModelSwitch switch_model(const Profile& p);
    void connect_profile(const Profile& p, bool power_cycle = true);
    bool connected() const { return app_.host_link().bridging(); }
    void new_connection();
    void edit_connection();
    void open_profile_file(const std::string& path, bool confirm_foreign = false);
    void connect_to_profile();
    bool save_current(bool save_as);
    bool confirm_discard();
    void edit_keymap();
    void edit_settings();
    void handle_trust_requests();
    void disconnect();
    void toggle_connection();
    void reboot();

    void burn_tick();
    void set_burn_speed(BurnSpeed speed);
    void replace_tube();
    void save_tube_file();

    void housekeeping();

    Application& app_;
    Emulator& emu_;
    QtLaunch& launch_;
    std::atomic<bool>& quit_;
    AppSettings& settings() { return launch_.settings; }
    const UserDirs& dirs() const { return *launch_.dirs; }

    ScreenWidget* screen_ = nullptr;
    std::shared_ptr<ScreenLink> link_;
    std::string model_name_;
    std::string title_, note_;
    bool quitting_ = false;

    QAction* save_ = nullptr;
    QAction* save_as_ = nullptr;
    QAction* disconnect_ = nullptr;
    QAction* zoom_in_ = nullptr;
    QAction* zoom_out_ = nullptr;
    std::vector<std::pair<double, QAction*>> zoom_actions_;
    std::vector<QAction*> burn_actions_;
    std::set<QKeySequence> shortcuts_;

    QLabel* profile_label_ = nullptr;
    QLabel* orb_[Profile::kSessions] = {};
    QLabel* lock_[Profile::kSessions] = {};
    SessionIndicator indicators_[Profile::kSessions];
    bool shown_connected_ = true;
    bool first_status_ = true;

    TubeWear tube_;
    steady::time_point tube_exposed_ = steady::now();
    steady::time_point tube_saved_ = steady::now();

    std::shared_ptr<TrustQueue> trust_ = std::make_shared<TrustQueue>();
    bool showing_trust_ = false;
    unsigned trust_station_ = 0;   // the session the open prompt is for
    bool trust_invalid_ = false;   // the open prompt is the invalid-certificate one
    CurrentProfile cur_;

    QTimer housekeeping_timer_;
    QTimer status_timer_;
    bool signal_seen_ = false;
};

MainWindow::MainWindow(Application& app, QtLaunch& launch, const FrontendOptions& opts, std::atomic<bool>& quit)
    : app_(app), emu_(app.emulator()), launch_(launch), quit_(quit) {
    model_name_ = app.model().info().name;
    {
        std::string tube_error;
        if (!load_tube(dirs().tube_file(), tube_, tube_error))
            LOG_WARN("ui", "%s: %s; starting with a new tube", dirs().tube_file().c_str(), tube_error.c_str());
    }
    screen_ = new ScreenWidget(emu_, opts.crt, this);
    screen_->is_shortcut = [this](const QKeySequence& k) { return shortcuts_.count(k) != 0; };
    screen_->set_tube(settings().burn_in == BurnSpeed::Off ? nullptr : &tube_);
    setCentralWidget(screen_);
    build_menus();
    build_toolbar();
    build_status_bar();

    // --- Picture: wake the UI thread when the emulator reports a change ----
    // (emulation thread; see ScreenLink).
    link_ = std::make_shared<ScreenLink>();
    link_->screen = screen_;
    emu_.set_display_callback([link = link_](uint64_t) {
        std::lock_guard<std::mutex> lock(link->mutex);
        if (!link->screen || link->wake_pending) return;
        link->wake_pending = true;
        QMetaObject::invokeMethod(
            link->screen,
            [link] {
                // UI thread, the only one that clears link->screen.
                ScreenWidget* s;
                {
                    std::lock_guard<std::mutex> inner(link->mutex);
                    link->wake_pending = false;
                    s = link->screen;
                }
                if (s) s->picture_changed();
            },
            Qt::QueuedConnection);
    });

    // --- TLS trust requests from the sessions' I/O threads ------------------
    trust_->attach(this, [this] { handle_trust_requests(); });
    {
        auto q = trust_;
        app_.host_link().set_tls_trust_handler([q](const tn5250::TlsTrustRequest& r) { return q->ask(r); });
    }

    QObject::connect(&housekeeping_timer_, &QTimer::timeout, this, [this] { housekeeping(); });
    housekeeping_timer_.start(kHousekeepingMs);
    QObject::connect(&status_timer_, &QTimer::timeout, this, [this] {
        guarded(this, [this] {
            handle_trust_requests();   // a prompt that waited for another dialog to close
            update_status();
            burn_tick();
        });
    });
    status_timer_.start(kStatusMs);

    restore_placement();
    update_ui_state();
}

MainWindow::~MainWindow() {
    // No new picture calls, then wait out a call in progress and cut the
    // link; a call still queued finds no screen.
    emu_.set_display_callback(nullptr);
    {
        std::lock_guard<std::mutex> lock(link_->mutex);
        link_->screen = nullptr;
    }
    trust_->close();
    app_.host_link().set_tls_trust_handler(nullptr);
}

// Menu commands run with the keys released, guarded, and hand the keyboard
// back to the screen.
QAction* MainWindow::add_action(QMenu* menu, const QString& text, std::function<void()> fn,
                                const QKeySequence& shortcut) {
    QAction* a = menu ? menu->addAction(text) : new QAction(text, this);
    if (!shortcut.isEmpty()) {
        a->setShortcut(shortcut);
        a->setShortcutContext(Qt::WindowShortcut);
        shortcuts_.insert(shortcut);
    }
    QObject::connect(a, &QAction::triggered, this, [this, fn = std::move(fn)] {
        screen_->release_keys();
        guarded(this, fn);
        if (isActiveWindow()) screen_->setFocus();
    });
    return a;
}

// Accelerators use Ctrl+Shift only; every other key belongs to the terminal.
void MainWindow::build_menus() {
    const auto cs = [](Qt::Key k) { return QKeySequence(Qt::CTRL | Qt::SHIFT | k); };
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* a = add_action(file, QStringLiteral("&New Connection..."), [this] { new_connection(); }, cs(Qt::Key_N));
    a->setIcon(glyph("new-connection"));
    a = add_action(file, QStringLiteral("&Connect to Profile..."), [this] { connect_to_profile(); }, cs(Qt::Key_O));
    a->setIcon(glyph("profiles"));
    file->addSeparator();
    save_ = add_action(file, QStringLiteral("&Save"), [this] { save_current(false); }, cs(Qt::Key_S));
    save_->setIcon(glyph("save"));
    save_as_ = add_action(file, QStringLiteral("Save &As..."), [this] { save_current(true); });
    file->addSeparator();
    disconnect_ = add_action(file, QStringLiteral("&Disconnect"), [this] { toggle_connection(); }, cs(Qt::Key_D));
    disconnect_->setIcon(glyph("disconnect"));
    file->addSeparator();
    add_action(file, QStringLiteral("E&xit"), [this] { close(); });

    QMenu* edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    add_action(edit, QStringLiteral("&Connection Settings..."), [this] { edit_connection(); }, cs(Qt::Key_E));
    a = add_action(edit, QStringLiteral("&Keymap..."), [this] { edit_keymap(); });
    a->setIcon(glyph("keymap"));
    a = add_action(edit, QStringLiteral("&Settings..."), [this] { edit_settings(); });
    a->setIcon(glyph("settings"));

    QMenu* view = menuBar()->addMenu(QStringLiteral("&View"));
    QMenu* zoom = view->addMenu(QStringLiteral("&Zoom"));
    auto* zoom_group = new QActionGroup(this);
    struct Level {
        const char* text;
        double factor;
    };
    for (const Level& l : {Level{"&1x", 1.0}, Level{"1&.5x", 1.5}, Level{"&2x", 2.0}}) {
        QAction* z = add_action(zoom, qs(l.text), [this, f = l.factor] {
            settings().zoom = f;
            apply_zoom();
            save_settings_file();
        });
        z->setCheckable(true);
        zoom_group->addAction(z);
        zoom_actions_.emplace_back(l.factor, z);
    }
    zoom->addSeparator();
    // The next level up, or the one below.
    auto step = [this](bool in) {
        double z = settings().zoom;
        for (double level : kZoomLevels) {
            if (in && level > settings().zoom + 0.01) {
                z = level;
                break;
            }
            if (!in && level < settings().zoom - 0.01) z = level;
        }
        settings().zoom = z;
        apply_zoom();
        save_settings_file();
    };
    zoom_in_ = add_action(zoom, QStringLiteral("Zoom &In"), [step] { step(true); });
    zoom_out_ = add_action(zoom, QStringLiteral("Zoom &Out"), [step] { step(false); });
    QMenu* burn = view->addMenu(QStringLiteral("&Burn-in"));
    auto* burn_group = new QActionGroup(this);
    int speed = 0;
    for (const char* label : {"&Off", "&Slow", "&Fast", "&Hyper"}) {
        QAction* b = add_action(burn, qs(label), [this, speed] { set_burn_speed(BurnSpeed(speed)); });
        b->setCheckable(true);
        burn_group->addAction(b);
        burn_actions_.push_back(b);
        ++speed;
    }
    burn->addSeparator();
    add_action(burn, QStringLiteral("&Replace CRT..."), [this] { replace_tube(); });
    // Radio items and enabled states follow the settings when the menu opens.
    QObject::connect(view, &QMenu::aboutToShow, this, [this] {
        const double z = settings().zoom;
        for (auto& [factor, action] : zoom_actions_)
            action->setChecked(factor == (z >= 2.0 ? 2.0 : z >= 1.5 ? 1.5 : 1.0));
        zoom_in_->setEnabled(z < 2.0);
        zoom_out_->setEnabled(z > 1.0);
        burn_actions_[size_t(settings().burn_in)]->setChecked(true);
    });

    QMenu* terminal = menuBar()->addMenu(QStringLiteral("&Terminal"));
    add_action(terminal, QStringLiteral("&Attn"), [this] { terminal_key("attn"); });
    add_action(terminal, QStringLiteral("&SysRq"), [this] { terminal_key("sysrq"); });
    add_action(terminal, QStringLiteral("&Reset"), [this] { terminal_key("reset"); });
    add_action(terminal, QStringLiteral("Set&up"), [this] { terminal_key("setup"); });
    terminal->addSeparator();
    a = add_action(terminal, QStringLiteral("Re&boot (Power Cycle)"), [this] { reboot(); });
    a->setIcon(glyph("reboot"));

    QMenu* help = menuBar()->addMenu(QStringLiteral("&Help"));
    add_action(help, QStringLiteral("Show &Log File"),
               [this] { open_with_desktop(this, dirs().root() + "/em34xx-qt.log"); });
    add_action(help, QStringLiteral("Open User &Folder"), [this] { open_with_desktop(this, dirs().root()); });
    help->addSeparator();
    add_action(help, QStringLiteral("&About..."), [this] { about_dialog(this, dirs()); });

    QObject::connect(file, &QMenu::aboutToShow, this, [this] { update_status(); });
}

void MainWindow::build_toolbar() {
    QToolBar* bar = addToolBar(QStringLiteral("Toolbar"));
    bar->setObjectName(QStringLiteral("toolbar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    // The menus' actions, with em34xxw's tooltips.
    auto find = [this](const char* text) -> QAction* {
        for (QAction* m : menuBar()->actions())
            for (QAction* a : m->menu()->actions())
                if (a->text() == qs(text)) return a;
        return nullptr;
    };
    auto tool = [bar](QAction* a, const char* tip) {
        if (!a) return;
        bar->addAction(a);
        a->setToolTip(qs(tip));
    };
    tool(find("&New Connection..."), "New Connection");
    tool(find("&Connect to Profile..."), "Connect to Profile");
    tool(save_, "Save Profile");
    bar->addSeparator();
    tool(disconnect_, "Disconnect");
    tool(find("Re&boot (Power Cycle)"), "Reboot (Power Cycle)");
    bar->addSeparator();
    tool(find("&Settings..."), "Settings");
    tool(find("&Keymap..."), "Keymap");
    bar->addSeparator();
    // Attn, SysRq, Reset and Setup show their names.
    for (const char* name : {"&Attn", "&SysRq", "&Reset", "Set&up"}) {
        QAction* a = find(name);
        if (!a) continue;
        bar->addAction(a);
        if (auto* b = qobject_cast<QToolButton*>(bar->widgetForAction(a))) b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    }
    // The keyboard stays with the screen: toolbar buttons take no focus.
    for (QAction* a : bar->actions())
        if (QWidget* w = bar->widgetForAction(a)) w->setFocusPolicy(Qt::NoFocus);
}

// Status bar: the profile on the left; on the right, per session an orb
// (its state) and a lock (TLS), each with the session's tooltip.
void MainWindow::build_status_bar() {
    profile_label_ = plain_label(this);
    statusBar()->addWidget(profile_label_, 1);
    auto* icons = new QWidget(this);
    auto* row = new QHBoxLayout(icons);
    row->setContentsMargins(0, 0, 4, 0);
    row->setSpacing(2);
    for (unsigned i = 0; i < Profile::kSessions; ++i) {
        if (i > 0) row->addSpacing(8);
        orb_[i] = new QLabel(icons);
        lock_[i] = new QLabel(icons);
        row->addWidget(orb_[i]);
        row->addWidget(lock_[i]);
    }
    statusBar()->addPermanentWidget(icons);
}

// The picture area becomes the presented frame times the zoom (logical
// pixels, so the desktop's scale applies), shrunk to fit the screen;
// resizing by hand stays possible and choosing a zoom snaps back.
void MainWindow::apply_zoom() {
    QSize want = screen_->presented_size() * settings().zoom;
    if (const QScreen* s = screen() ? screen() : QGuiApplication::primaryScreen()) {
        const QSize extra = size() - screen_->size();
        const QSize avail = s->availableGeometry().size() - extra - QSize(16, 48);
        if (want.width() > avail.width() || want.height() > avail.height()) want.scale(avail, Qt::KeepAspectRatio);
    }
    if (isMaximized() || isFullScreen()) showNormal();
    screen_->set_size_hint(want);
    // Only the picture's hint changes: the window grows or shrinks by the
    // difference, the bars keep their size.
    resize(size() - screen_->size() + want);
}

void MainWindow::restore_placement() {
    const WindowPlacement& wp = settings().window;
    if (wp.valid && wp.width > 0 && wp.height > 0) {
        const QRect r(wp.x, wp.y, wp.width, wp.height);
        // On a screen as it is now (resolution or layout may have changed).
        for (const QScreen* s : QGuiApplication::screens()) {
            const QRect avail = s->availableGeometry();
            if (avail.intersects(r) && wp.width <= avail.width() && wp.height <= avail.height()) {
                resize(wp.width, wp.height);
                // Wayland does not let a client place its window: the
                // compositor does (the position is ignored there).
                // The last position at which the whole window is on the
                // screen: x + width - 1 <= right, so x <= left + avail
                // width - width (>= left, as width <= avail width). A
                // window as wide as the screen sits at its left edge.
                const int max_x = avail.x() + avail.width() - wp.width;
                const int max_y = avail.y() + avail.height() - wp.height;
                move(std::max(avail.x(), std::min(wp.x, max_x)), std::max(avail.y(), std::min(wp.y, max_y)));
                return;
            }
        }
    }
    // Before the window is shown its bars have no size yet: lay out first.
    adjustSize();
    apply_zoom();
}

void MainWindow::save_placement() {
    WindowPlacement& s = settings().window;
    const QRect r = isMaximized() || isFullScreen() ? normalGeometry() : QRect(pos(), size());
    if (r.width() <= 0 || r.height() <= 0) return;
    s.valid = true;
    s.x = r.x();
    s.y = r.y();
    s.width = r.width();
    s.height = r.height();
    s.maximized = isMaximized();
}

void MainWindow::save_settings_file() {
    save_placement();
    std::string error;
    if (!save_settings(dirs().settings_file(), settings(), error)) LOG_WARN("ui", "%s", error.c_str());
}

// Press and release one of the terminal's own keys (Attn, SysRq, Reset,
// Setup) through the keyboard model.
void MainWindow::terminal_key(const char* function) {
    const bool ok = emu_.with_machine([&](Machine& m) {
        if (!m.terminal_key(function, true)) return false;
        m.terminal_key(function, false);
        return true;
    });
    if (!ok) LOG_WARN("input", "this keyboard has no %s key", function);
}

// ---------------------------------------------------------------------------
// Title and status

std::string MainWindow::profile_display_name() const {
    if (!cur_.active) return std::string();
    return (cur_.untitled ? std::string(kUntitled) : cur_.name) + (cur_.modified ? "*" : "");
}

void MainWindow::update_title() {
    std::string t;
    const std::string prof = profile_display_name();
    if (!prof.empty()) t = prof + " - ";
    t += title_.empty() ? "IBM " + app_.variant().id + " - em34xx" : title_;
    if (!note_.empty()) t += " - " + note_;
    setWindowTitle(qs(t));
}

std::string MainWindow::tls_state_text(const TwinaxSessionStatus* s) const {
    if (cur_.p.tls_mode == ProfileTlsMode::AcceptAll) return "TLS, certificate not verified";
    if (!s || !s->connected) return "TLS";
    if (s->address < Profile::kSessions && cur_.overridden[s->address])
        return "TLS, certificate NOT VALID (accepted for this connection)";
    switch (cur_.p.tls_mode) {
    case ProfileTlsMode::System: return cur_.accepted_once ? "TLS, certificate accepted once" : "TLS, verified";
    case ProfileTlsMode::AcceptAll: break;
    case ProfileTlsMode::Fingerprint:
        return cur_.accepted_once ? "TLS, certificate accepted once" : "TLS, trusted certificate";
    }
    return "TLS";
}

SessionIndicator MainWindow::session_indicator(unsigned i, const std::vector<TwinaxSessionStatus>& all) const {
    SessionIndicator ind;
    const std::string label = format("Session %u", i + 1);
    if (!cur_.active || !cur_.p.sessions[i].used()) {
        ind.tip = label + ": not configured";
        return ind;
    }
    const ProfileSession& ps = cur_.p.sessions[i];
    const TwinaxSessionStatus* s = nullptr;
    for (const TwinaxSessionStatus& x : all)
        if (!x.printer && x.address == i) s = &x;
    const bool tls = s ? s->tls : ps.tls;
    // Encrypted but with any certificate accepted: not the plain closed lock.
    ind.lock = !tls ? "lock-open" : cur_.p.tls_mode == ProfileTlsMode::AcceptAll ? "lock-closed-dim" : "lock-closed";
    std::string state;
    if (showing_trust_ && trust_station_ == i) {
        ind.orb = "orb-orange";
        state = trust_invalid_ ? "connecting, the certificate is not valid: waiting for your answer"
                               : "connecting, the certificate is not trusted: waiting for your answer";
    } else if (!s) {
        ind.orb = "orb-red";
        state = "disconnected";
    } else if (s->connected && s->state == "ready") {
        ind.orb = "orb-green";
        state = "connected";
    } else if (s->held || s->state == "closed" || s->state == "failed" || s->state == "disconnected") {
        ind.orb = "orb-red";
        state = s->held ? "disconnected" : s->state == "failed" ? "failed" : "closed";
        if (!s->error.empty()) state += " (" + s->error + ")";
        if (!s->held) state += ", retrying";
    } else {
        ind.orb = "orb-orange";
        state = s->state.empty() ? "connecting" : s->state;
    }
    const std::string peer = s && !s->peer.empty() ? s->peer : format("%s:%u", ps.host.c_str(), unsigned(ps.port));
    ind.tip = label + ": " + peer + "\n" + state + "\n" + (tls ? tls_state_text(s) : "TLS off (plain telnet)");
    return ind;
}

void MainWindow::update_status() {
    const std::vector<TwinaxSessionStatus> sessions = app_.host_link().sessions();
    profile_label_->setText(cur_.active ? qs("Profile: " + profile_display_name()) : QStringLiteral("no profile"));
    // The glyphs follow the text height (16 px at the usual font size).
    const int size = std::clamp(profile_label_->fontMetrics().height(), 16, 32);
    for (unsigned i = 0; i < Profile::kSessions; ++i) {
        const SessionIndicator ind = session_indicator(i, sessions);
        if (!first_status_ && ind == indicators_[i]) continue;
        orb_[i]->setPixmap(glyph(ind.orb).pixmap(QSize(size, size), devicePixelRatioF()));
        lock_[i]->setPixmap(glyph(ind.lock).pixmap(QSize(size, size), devicePixelRatioF()));
        orb_[i]->setToolTip(plain_tip(qs(ind.tip)));
        lock_[i]->setToolTip(plain_tip(qs(ind.tip)));
        indicators_[i] = ind;
    }
    first_status_ = false;
    // Disconnect / Reconnect: one command, its text and picture follow the
    // state.
    const bool on_line = connected();
    if (on_line != shown_connected_) {
        shown_connected_ = on_line;
        disconnect_->setText(on_line ? QStringLiteral("&Disconnect") : QStringLiteral("&Reconnect"));
        disconnect_->setIcon(glyph(on_line ? "disconnect" : "reconnect"));
        disconnect_->setToolTip(on_line ? QStringLiteral("Disconnect") : QStringLiteral("Reconnect"));
    }
    disconnect_->setEnabled(on_line || cur_.active);
    save_->setEnabled(cur_.active);
    save_as_->setEnabled(cur_.active);
}

void MainWindow::update_ui_state() {
    update_title();
    update_status();
}

// ---------------------------------------------------------------------------
// Profiles and connections

const Model* MainWindow::profile_model(const Profile& p, const std::string& what) {
    const Model* model = nullptr;
    for (const Model* m : runnable_models())
        if (m->info().name == p.model) model = m;
    if (!model) {
        error_box(this, what + " is for the IBM " + p.model + ", which this version of em34xx cannot emulate.");
        return nullptr;
    }
    return ensure_model_roms(this, dirs(), *model, what, &model->variant_named(p.variant)) ? model : nullptr;
}

MainWindow::ModelSwitch MainWindow::switch_model(const Profile& p) {
    // The same unit: the model and its variant (a profile that names none
    // means the model's first).
    Profile want = p;
    want.normalize();
    if (want.model == model_name_ && want.variant == app_.variant().id) return ModelSwitch::Same;
    const Model* model = profile_model(p, "This connection");
    if (!model) return ModelSwitch::Failed;
    // The new terminal as the profile describes it, with its own
    // battery-backed RAM file (its setup); the keyboard stays.
    TerminalOptions options = emu_.with_machine([](Machine& m) { return m.options(); });
    options.variant = want.variant;
    options.phosphor = want.phosphor;
    // A variant with its own firmware keeps its own (Model::storage_name).
    const std::string stored_as = model->storage_name(model->variant_named(want.variant));
    options.nvram_path = dirs().nvram_file(stored_as);
    options.eeprom_path = dirs().eeprom_file(stored_as);
    std::string error;
    bool ok = false;
    {
        TrustDrain drain(*trust_);   // the rebuild closes the sessions
        screen_->release_keys();
        ok = app_.set_model(*model, options, error);
    }
    if (!ok) {
        error_box(this, "Cannot start the IBM " + want.variant + ": " + error);
        return ModelSwitch::Failed;
    }
    model_name_ = model->info().name;
    screen_->redraw();
    update_ui_state();
    return ModelSwitch::Switched;
}

// The profile's CRT type (a monochrome unit's tube: the picture only, the
// terminal keeps running) and scan lines. Another variant is another unit:
// switch_model.
bool MainWindow::apply_terminal_settings(const Profile& p) {
    if (screen_->crt().enabled != p.crt) {
        CrtParams crt = screen_->crt();
        crt.enabled = p.crt;
        app_.set_crt(crt);
        screen_->set_crt(crt);
    }
    Profile want = p;
    want.normalize();
    if (!want.phosphor.empty() && want.variant == app_.variant().id) {
        std::string error;
        if (!app_.set_phosphor(want.phosphor, error)) {
            error_box(this, std::string("Cannot change the CRT type: ") + error);
            return false;
        }
    }
    screen_->redraw();
    return true;
}

void MainWindow::connect_profile(const Profile& p, bool power_cycle) {
    // Another terminal model: the emulator restarts as it. Without its ROM
    // images nothing connects (the sessions would announce the wrong type).
    const ModelSwitch switched = switch_model(p);
    if (switched == ModelSwitch::Failed) {
        update_ui_state();
        return;
    }
    AppConfig cfg;
    std::string error, message;
    if (!profile_connect_config(p, app_.host_link().base(), settings().tls_min_version, cfg, error)) {
        error_box(this, "Cannot connect: " + error);
        return;
    }
    const bool rebuilt = switched == ModelSwitch::Switched;
    {
        // The running sessions close here, with the trust queue draining:
        // their threads are joined, and one that reaches its certificate
        // check meanwhile is refused instead of waiting for a prompt. The
        // new sessions start after it and may ask again.
        TrustDrain drain(*trust_);
        apply_terminal_settings(p);
        screen_->release_keys();
        std::string ignored;
        app_.host_link().disconnect(ignored);   // "not connected" is fine
    }
    cur_.accepted_once = false;
    for (bool& o : cur_.overridden) o = false;
    // Like switching the terminal on: the running sessions close, the
    // terminal runs its power-on self test, and the controller brings the
    // sessions up once it answers the polls. A rebuilt machine is already
    // powering on.
    const bool ok = power_cycle && !rebuilt ? app_.connect_after_power_cycle(cfg, message, error)
                                            : app_.host_link().connect(cfg, message, error);
    if (!ok) {
        error_box(this, error);
        update_ui_state();
        return;
    }
    LOG_INFO("tn5250", "%s", trim(message).c_str());
    update_ui_state();
}

void MainWindow::new_connection() {
    Profile p;
    p.model = model_name_;
    p.variant = app_.variant().id;
    p.phosphor = app_.config().terminal.phosphor;
    if (!profile_editor_dialog(this, p, ProfileEditMode::Connect, settings(), dirs(), "New Connection")) return;
    if (!profile_model(p, "This connection")) return;
    if (!confirm_discard()) return;
    cur_ = CurrentProfile();
    cur_.active = true;
    cur_.p = p;
    cur_.untitled = true;
    cur_.modified = true;   // never saved
    connect_profile(cur_.p);
}

// Edit > Connection Settings: the running profile in the editor. Changes
// mark it modified and apply now: the CRT type at once (or after a
// confirmation when the terminal must restart), connection changes by
// reconnecting the sessions with the terminal running.
void MainWindow::edit_connection() {
    if (!cur_.active) {
        new_connection();
        return;
    }
    Profile p = cur_.p;
    const std::string title = "Connection Settings: " + (cur_.untitled ? std::string(kUntitled) : cur_.name);
    if (!profile_editor_dialog(this, p, ProfileEditMode::Apply, settings(), dirs(), title)) return;
    if (p == cur_.p) return;
    Profile old = cur_.p;
    old.normalize();
    p.normalize();
    // Another terminal type (model or variant): it needs its ROM images and
    // a restart; refused or not possible, the terminal stays and the other
    // changes apply.
    if ((p.model != model_name_ || p.variant != app_.variant().id) &&
        (!profile_model(p, "This connection") ||
         !ask_yes_no(this, "Changing the terminal type to IBM " + p.variant +
                               " restarts the emulator as that terminal and reconnects the sessions. Continue?"))) {
        p.model = old.model;
        p.variant = old.variant;
        p.phosphor = old.phosphor;
        p.normalize();
    }
    Profile same_terminal = old;
    same_terminal.model = p.model;
    same_terminal.variant = p.variant;
    same_terminal.phosphor = p.phosphor;
    same_terminal.crt = p.crt;
    const bool connection_changed = same_terminal != p;
    const bool was_connected = connected();
    const ModelSwitch switched = switch_model(p);
    if (switched == ModelSwitch::Failed) {
        p.model = old.model;
        p.variant = old.variant;
        p.phosphor = old.phosphor;
    }
    if (!apply_terminal_settings(p)) p.phosphor = app_.config().terminal.phosphor;
    const bool rebuilt = switched == ModelSwitch::Switched;
    if (p != old) {
        cur_.p = p;
        cur_.modified = true;
    }
    // A rebuilt terminal lost its sessions; changed sessions reconnect.
    if (was_connected && (rebuilt || connection_changed)) connect_profile(cur_.p, false);
    update_ui_state();
}

void MainWindow::disconnect() {
    {
        TrustDrain drain(*trust_);
        std::string error;
        app_.host_link().disconnect(error);   // "not connected" is fine
    }
    update_ui_state();
}

// File > Disconnect, and off line Reconnect: the current profile again
// without a power cycle (the terminal keeps running, as when plugging the
// cable back in).
void MainWindow::toggle_connection() {
    if (connected()) disconnect();
    else if (cur_.active) connect_profile(cur_.p, false);
}

// Terminal > Reboot: switch the terminal off and on. Its sessions end, as
// on a real host; the terminal runs its self test and comes back on line
// to new ones. After a Disconnect, it also dials the host again.
void MainWindow::reboot() { app_.power_on(); }

void MainWindow::open_profile_file(const std::string& path, bool confirm_foreign) {
    Profile p;
    std::string error;
    if (!load_profile(path, p, error)) {
        error_box(this, error);
        return;
    }
    if (confirm_foreign && !file_in_folder(path, dirs().profiles()) &&
        !ask_yes_no(this, foreign_profile_question(path, p), true, true))
        return;
    // A profile for a terminal whose ROM images are missing: the user is
    // told which files and may import them; otherwise nothing changes.
    if (!profile_model(p, "This profile")) return;
    if (!confirm_discard()) return;
    cur_ = CurrentProfile();
    cur_.active = true;
    cur_.p = p;
    cur_.name = file_stem(path);
    cur_.path = path;
    connect_profile(cur_.p);
}

void MainWindow::connect_to_profile() {
    const std::string name = profile_manager_dialog(this, dirs(), settings());
    if (!name.empty()) open_profile_file(dirs().profile_path(name));
}

// Save (or Save As) the current profile. False when cancelled or failed.
bool MainWindow::save_current(bool save_as) {
    if (!cur_.active) return false;
    if (cur_.untitled || save_as) {
        std::string name = cur_.untitled ? dirs().unique_profile_name(cur_.p.sessions[0].host) : cur_.name;
        if (!name_dialog(this, "Save Profile As", "&Profile name:", name)) return false;
        if (dirs().profile_exists(name) && (cur_.untitled || name != cur_.name) &&
            !ask_yes_no(this, "A profile named \"" + name + "\" exists. Replace it?"))
            return false;
        std::string error;
        if (!save_profile(dirs().profile_path(name), cur_.p, error)) {
            error_box(this, error);
            return false;
        }
        cur_.name = name;
        cur_.path = dirs().profile_path(name);
        cur_.untitled = false;
    } else {
        std::string error;
        if (!save_profile(cur_.path, cur_.p, error)) {
            error_box(this, error);
            return false;
        }
    }
    cur_.modified = false;
    update_ui_state();
    return true;
}

// Before the current profile is replaced or the program ends: offer to
// save unsaved changes. False: the user cancelled.
bool MainWindow::confirm_discard() {
    if (!cur_.active || !cur_.modified) return true;
    const std::string name = cur_.untitled ? std::string(kUntitled) : cur_.name;
    QMessageBox box(QMessageBox::Question, app_title(), qs("Save the changes to the profile \"" + name + "\"?"),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setDefaultButton(QMessageBox::Save);
    box.setTextFormat(Qt::PlainText);
    const int r = box.exec();
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return save_current(false);
    return true;
}

void MainWindow::handle_trust_requests() {
    if (showing_trust_) return;   // the loop below picks up later requests
    // Another dialog or message box is up: the prompt waits until it has
    // closed, so it never opens over a modal dialog and never changes the
    // profile under an open editor. The status timer comes back here.
    if (trust_->empty() || QApplication::activeModalWidget() || quitting_) return;
    showing_trust_ = true;
    screen_->release_keys();
    while (auto p = trust_->take()) {
        const std::string name =
            cur_.active ? (cur_.untitled ? std::string(kUntitled) : cur_.name) : std::string("(none)");
        trust_station_ = p->req.station;
        trust_invalid_ = p->req.problem == tn5250::TlsTrustProblem::Invalid;
        update_status();   // that session's orb: waiting for the answer
        // A certificate that is not valid has its own prompt, with no way
        // to remember the answer: nothing below touches the profile for it,
        // and every connection attempt comes back here.
        const tn5250::TlsTrustDecision d =
            trust_invalid_ ? (certificate_invalid_dialog(this, p->req) ? tn5250::TlsTrustDecision::AcceptOnce
                                                                       : tn5250::TlsTrustDecision::Reject)
                           : trust_dialog(this, p->req, name);
        // The answer first: a session that stopped waiting (the ten minutes
        // ran out, or it was closed) has already been refused, and then the
        // answer changes nothing, the profile least of all.
        if (!trust_->answer(p, d)) {
            if (d != tn5250::TlsTrustDecision::Reject)
                info_box(this, format("The connection to %s:%u waited too long for an answer and was refused; "
                                      "nothing was changed. It asks again when it reconnects.",
                                      printable(p->req.host).c_str(), unsigned(p->req.port)));
        } else if (trust_invalid_) {
            // A certificate that is not valid: nothing to record, and every
            // connection attempt asks again.
            if (p->req.station < Profile::kSessions)
                cur_.overridden[p->req.station] = d == tn5250::TlsTrustDecision::AcceptOnce;
        } else {
            // A later connection of this session that needed no override.
            if (p->req.station < Profile::kSessions && d != tn5250::TlsTrustDecision::Reject)
                cur_.overridden[p->req.station] = false;
            if (d == tn5250::TlsTrustDecision::AcceptPermanently && cur_.active) {
                profile_trust_certificate(cur_.p, p->req.station, p->req.peer.sha256);
                std::string error;
                if (!cur_.untitled && save_profile(cur_.path, cur_.p, error)) {
                    cur_.modified = false;
                } else {
                    cur_.modified = true;
                    if (!error.empty()) error_box(this, error);
                }
            } else if (d == tn5250::TlsTrustDecision::AcceptOnce) {
                cur_.accepted_once = true;
            }
        }
        // Rejected: keep the session closed instead of reconnecting into the
        // same question (after the I/O thread has had its answer).
        if (d == tn5250::TlsTrustDecision::Reject) {
            const unsigned station = p->req.station;
            QTimer::singleShot(0, this, [this, station] {
                std::string error;
                app_.host_link().disconnect_station(station, error);
                update_status();
            });
        }
        update_ui_state();
    }
    showing_trust_ = false;
    update_status();
    screen_->setFocus();
}

void MainWindow::edit_keymap() {
    KeymapFile file = pcomm_keymap_file();
    std::string text, error;
    std::error_code ec;
    if (std::filesystem::exists(dirs().keymap_file(), ec)) {
        if (!read_text_file(dirs().keymap_file(), text, error) ||
            !parse_keymap_file(text, dirs().keymap_file(), file, error)) {
            error_box(this, error + "\n\nThe keymap editor starts from the default (PCOMM) keymap.");
            file = pcomm_keymap_file();
        }
    }
    if (!keymap_dialog(this, file)) return;
    if (!write_text_file(dirs().keymap_file(),
                         keymap_file_to_toml(file, "em34xx host keymap (written by em34xx-qt; docs/keyboard.md).\n"
                                                   "Applies to every profile."),
                         error)) {
        error_box(this, error);
        return;
    }
    launch_.keymap_path = dirs().keymap_file();
    if (!emu_.with_machine([&](Machine& m) { return m.set_keymap(launch_.keymap_path, error); }))
        error_box(this, "The keymap was saved but could not be applied: " + error);
}

void MainWindow::edit_settings() {
    AppSettings s = settings();
    if (!settings_dialog(this, dirs(), s)) return;
    const bool speed_changed = s.speed_percent != settings().speed_percent;
    s.window = settings().window;
    s.zoom = settings().zoom;
    settings() = s;
    if (speed_changed) app_.set_speed_percent(s.speed_percent);
    screen_->set_tube(settings().burn_in == BurnSpeed::Off ? nullptr : &tube_);
    std::string error;
    if (!save_settings(dirs().settings_file(), settings(), error)) error_box(this, error);
}

// The picture now on the tube has been there since the last tick. A gap
// longer than a few ticks (the machine slept) is not counted. The timer
// also runs while a dialog is up, so that time counts: the tube is lit.
void MainWindow::burn_tick() {
    const auto now = steady::now();
    const double seconds = std::min(2.0, std::chrono::duration<double>(now - tube_exposed_).count());
    tube_exposed_ = now;
    const uint64_t before = tube_.version();
    tube_.expose(screen_->framebuffer(), seconds, settings().burn_in);
    if (tube_.version() != before && settings().burn_in != BurnSpeed::Off) screen_->redraw();
    if (now - tube_saved_ >= std::chrono::minutes(5)) save_tube_file();
}

void MainWindow::save_tube_file() {
    tube_saved_ = steady::now();
    std::string error;
    if (!save_tube(dirs().tube_file(), tube_, error)) LOG_WARN("ui", "%s", error.c_str());
}

void MainWindow::set_burn_speed(BurnSpeed speed) {
    settings().burn_in = speed;
    screen_->set_tube(speed == BurnSpeed::Off ? nullptr : &tube_);
    std::string error;
    if (!save_settings(dirs().settings_file(), settings(), error)) error_box(this, error);
}

// View > Burn-in > Replace CRT: a new tube.
void MainWindow::replace_tube() {
    if (!ask_yes_no(this, "Replace the CRT with a new one? The burn-in of this tube is lost.")) return;
    tube_.reset();
    save_tube_file();
    screen_->redraw();
}

// Once the window is up. A profile on the command line connects (asked
// about when it is not in the profiles folder); otherwise the startup
// profile (Settings) when one is set and still exists; else the profile
// manager. Cancel leaves the terminal idle.
void MainWindow::startup() {
    if (quit_.load() || quitting_) return;
    if (!launch_.profile_path.empty() && !launch_.skip_start_profile) {
        open_profile_file(launch_.profile_path, true);
        return;
    }
    // The name is a file name in profiles/ and nothing else (it comes from
    // settings.toml): never a path.
    if (std::string why; !launch_.skip_start_profile && settings().startup_open &&
                         !settings().startup_profile.empty() &&
                         UserDirs::valid_profile_name(settings().startup_profile, why) &&
                         dirs().profile_exists(settings().startup_profile)) {
        open_profile_file(dirs().profile_path(settings().startup_profile));
    } else {
        connect_to_profile();
    }
    screen_->setFocus();
}

void MainWindow::housekeeping() {
    if (g_signal != 0 && !signal_seen_) {
        signal_seen_ = true;
        LOG_INFO("ui", "signal %d: closing the window", int(g_signal));
    }
    if (emu_.quit_requested() || quit_.load() || signal_seen_) {
        // Asked to end: no questions. Also out of a dialog's own event loop.
        quitting_ = true;
        cur_.modified = false;
        QApplication::closeAllWindows();
        QCoreApplication::quit();
        return;
    }
    screen_->poll_generation();
}

bool MainWindow::query_end_session() {
    screen_->release_keys();
    return confirm_discard();
}

// The desktop session ends: the process may be ended without a clean exit.
// Write what a clean exit writes: the terminal's battery-backed RAM and
// setup EEPROM, the tube's wear, the window placement and the settings.
void MainWindow::end_session() {
    std::string error;
    app_.flush_persistent_state(error);   // logs a failure itself
    save_tube_file();
    save_settings_file();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    screen_->release_keys();
    if (!quitting_ && !confirm_discard()) {
        e->ignore();
        return;
    }
    quitting_ = true;
    {
        // Sessions waiting on a prompt are answered Stop.
        TrustDrain drain(*trust_);
    }
    save_tube_file();
    save_settings_file();
    e->accept();
}

void MainWindow::changeEvent(QEvent* e) {
    if (e->type() == QEvent::ActivationChange) {
        if (!isActiveWindow()) screen_->release_keys();
        else if (!QApplication::activeModalWidget()) screen_->setFocus();
    }
    QMainWindow::changeEvent(e);
}

// ---------------------------------------------------------------------------
// Frontend

class QtFrontend : public Frontend {
public:
    explicit QtFrontend(QtLaunch& launch) : launch_(launch) {}

    const char* name() const override { return "qt"; }
    bool run(Application& app, const FrontendOptions& opts, std::string& error) override;
    void request_quit() override { quit_.store(true); }
    void set_title(const std::string& title) override {
        std::lock_guard<std::mutex> lock(text_mutex_);
        title_ = title;
        text_changed_ = true;
    }
    void set_status(const std::string& status) override {
        std::lock_guard<std::mutex> lock(text_mutex_);
        status_ = status;
        text_changed_ = true;
    }

private:
    QtLaunch& launch_;
    std::atomic<bool> quit_{false};
    std::mutex text_mutex_;
    std::string title_;
    std::string status_;
    bool text_changed_ = true;
};

bool QtFrontend::run(Application& app, const FrontendOptions& opts, std::string& error) {
    if (!launch_.qapp) {
        error = "the Qt application was not set up (qt_prepare)";
        return false;
    }
    SignalScope signals;
    LOG_INFO("ui", "Qt %s, platform %s, style %s", qVersion(), qPrintable(QGuiApplication::platformName()),
             qPrintable(QApplication::style()->objectName()));

    MainWindow window(app, launch_, opts, quit_);
    // Title and status note from any thread: picked up by the UI thread.
    QTimer texts;
    QObject::connect(&texts, &QTimer::timeout, &window, [&] {
        std::lock_guard<std::mutex> lock(text_mutex_);
        if (!text_changed_) return;
        text_changed_ = false;
        window.set_texts(title_, status_);
    });
    texts.start(kHousekeepingMs);

#ifndef QT_NO_SESSIONMANAGER
    // Log-out or shutdown: an unsaved or changed profile is asked about
    // first, as on exit (Cancel asks the session to stay); then the state a
    // clean exit writes is written, since the process may end at once.
    QObject::connect(launch_.qapp.get(), &QGuiApplication::commitDataRequest, &window, [&](QSessionManager& sm) {
        if (sm.allowsInteraction()) {
            const bool ok = window.query_end_session();
            sm.release();
            if (!ok) {
                sm.cancel();
                return;
            }
        }
        window.end_session();
    });
#endif

    if (settings_maximized(launch_.settings)) window.showMaximized();
    else window.show();
    QTimer::singleShot(0, &window, [&] { guarded(&window, [&] { window.startup(); }); });

    // Returns when the window closes.
    QApplication::exec();
    if (!app.emulator().quit_requested()) app.emulator().request_quit(app.emulator().exit_code());
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Launch

namespace {

// Qt reads its own options (-platform, -style...) from the command line;
// ours is not Qt's, so Qt gets the program name only.
char g_arg0[] = "em34xx-qt";
char* g_qt_argv[] = {g_arg0, nullptr};
int g_qt_argc = 1;

// --- Can Qt start? ---------------------------------------------------------
//
// Without a usable platform plugin (a display that cannot be reached, a bad
// QT_QPA_PLATFORM, no X11 display and no Wayland socket) the QApplication
// constructor does not fail: it calls qFatal, which ends the process with
// abort() (documented: QGuiApplication, "-platform"; qFatal). An unset
// WAYLAND_DISPLAY is not enough for that: Qt's wayland plugin then connects
// to the default socket, wayland-0 in XDG_RUNTIME_DIR (observed). Qt offers
// no way to ask first, so while the constructor runs a message handler
// collects Qt's messages and, on the fatal one, reports them and leaves
// with _exit(2): no abort(), no core dump. Nothing needs cleaning up at that
// point (no emulator, no files open for writing).

std::string g_startup_messages;   // Qt's messages while the platform starts
QtMessageHandler g_previous_handler = nullptr;

std::string no_display_error(const std::string& detail) {
    auto env = [](const char* name) {
        const char* v = std::getenv(name);
        return std::string(name) + (v ? std::string("=") + v : std::string(" is not set"));
    };
    return "Qt cannot open a display (" + env("WAYLAND_DISPLAY") + ", " + env("DISPLAY") + ", " +
           env("QT_QPA_PLATFORM") + "): " + detail;
}

void startup_message(QtMsgType type, const QMessageLogContext& context, const QString& text) {
    if (type != QtFatalMsg) {
        // Shown as usual (a theme warning, say), and kept for the error.
        if (!g_startup_messages.empty()) g_startup_messages += ' ';
        g_startup_messages += text.simplified().toStdString();
        if (g_previous_handler) g_previous_handler(type, context, text);
        else std::fprintf(stderr, "%s\n", text.toLocal8Bit().constData());
        return;
    }
    // One line: the messages before it and the fatal one.
    std::string detail = g_startup_messages;
    if (!detail.empty()) detail += ' ';
    detail += text.simplified().toStdString();
    std::fprintf(stderr, "em34xx-qt: %s\n", no_display_error(detail).c_str());
    std::fflush(stderr);
    ::_exit(2);
}

// The log levels: "info", or EM34XX_LOG (the same spec as --log, e.g.
// "info,wsc=debug,tn5250=debug") to diagnose input or a host session.
std::string log_spec() {
    const char* env = std::getenv("EM34XX_LOG");
    return env && *env ? std::string(env) : std::string("info");
}

const char kUsage[] = "Usage: em34xx-qt [PROFILE.em34xx]\n"
                      "\n"
                      "The IBM 34xx twinax terminal emulator, connected to IBM i over TN5250.\n"
                      "PROFILE  a connection profile to open and connect at start\n"
                      "\n"
                      "  --help       this text\n"
                      "  --version    the version\n"
                      "\n"
                      "Settings, ROM images, the keymap and profiles are kept in\n"
                      "$XDG_DATA_HOME/em34xx (~/.local/share/em34xx), or the folder named by\n"
                      "EM34XX_USER_DIR. EM34XX_LOG sets the log levels (as em34xx --log).\n"
                      "See docs/frontend-qt.md.\n";

// Errors before the window: a message box, and standard error.
void prepare_error(const std::string& message) {
    std::fprintf(stderr, "em34xx-qt: %s\n", message.c_str());
    error_box(nullptr, message);
}

} // namespace

QtLaunch::QtLaunch() = default;
QtLaunch::~QtLaunch() = default;

bool qt_prepare(int argc, char** argv, QtLaunch& launch) {
    // The command line: an optional .em34xx profile to open and connect.
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            std::fputs(kUsage, stdout);
            return false;
        }
        if (a == "--version") {
            std::printf("em34xx-qt %s\n", EM34XX_VERSION_STRING);
            return false;
        }
        if (launch.profile_path.empty() && !a.empty() && a[0] != '-') {
            launch.profile_path = a;
            continue;
        }
        std::fprintf(stderr, "em34xx-qt: unknown command-line argument: %s\n\n%s", a.c_str(), kUsage);
        launch.exit_code = 2;
        return false;
    }

    QApplication::setApplicationName(QStringLiteral("em34xx"));
    QApplication::setApplicationVersion(QStringLiteral(EM34XX_VERSION_STRING));
    QApplication::setDesktopFileName(QStringLiteral("em34xx"));
    // A platform that cannot start ends the program here, with exit code 2
    // (startup_message).
    g_startup_messages.clear();
    g_previous_handler = qInstallMessageHandler(startup_message);
    launch.qapp = std::make_unique<QApplication>(g_qt_argc, g_qt_argv);
    qInstallMessageHandler(g_previous_handler);
    // The window icon is built in, so it shows without an installed icon
    // theme: em34xxw's pixel art, each size its own drawing or a whole-number
    // enlargement (tools/gen_linux_assets.py).
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 96, 128, 192, 256})
        icon.addFile(QStringLiteral(":/icons/%1x%1/apps/em34xx.png").arg(size), QSize(size, size));
    QApplication::setWindowIcon(icon);
    if (launch.on_qapp) launch.on_qapp();

    // The user folder: $XDG_DATA_HOME/em34xx, or EM34XX_USER_DIR.
    const std::string root = user_folder();
    if (root.empty()) {
        prepare_error("Cannot find the user's data folder (neither EM34XX_USER_DIR, XDG_DATA_HOME nor HOME is set).");
        launch.exit_code = 2;
        return false;
    }
    launch.dirs = std::make_unique<UserDirs>(root);
    std::string error;
    if (!launch.dirs->ensure(error)) {
        prepare_error(error);
        launch.exit_code = 2;
        return false;
    }
    if (!load_settings(launch.dirs->settings_file(), launch.settings, error)) {
        error_box(nullptr, error + "\n\nDefault settings are used.");
        launch.settings = AppSettings();
    }

    // The terminal starts as the profile that will be opened describes it
    // (the command-line profile, else the startup profile: model, CRT type,
    // scan lines), else as a new profile would: the first model whose ROM
    // images are there, green, with scan lines. Connecting applies a
    // profile's settings in any case.
    if (runnable_models().empty()) {
        prepare_error("No terminal model of this build can run yet.");
        launch.exit_code = 2;
        return false;
    }
    Profile start;
    std::string start_name;   // "" = no profile asks for a model
    {
        std::string path = launch.profile_path, why, ignored;   // load errors: reported when it is opened
        if (path.empty() && launch.settings.startup_open && !launch.settings.startup_profile.empty() &&
            UserDirs::valid_profile_name(launch.settings.startup_profile, why) &&
            launch.dirs->profile_exists(launch.settings.startup_profile))
            path = launch.dirs->profile_path(launch.settings.startup_profile);
        if (!path.empty()) {
            if (load_profile(path, start, ignored)) start_name = file_stem(path);
            else start = Profile();
        }
    }

    // One complete set of ROM images is enough to start; the other models
    // stay selectable (greyed) until theirs are imported. A profile for a
    // model without its images: say which files, offer to import them, and
    // otherwise start without the profile.
    const std::string roms = launch.dirs->roms();
    const std::string wanted = start.variant.empty() ? start.model : start.variant;
    StartModel choice = choose_start_model(start_name.empty() ? std::string() : wanted, roms);
    bool declined = false;
    if (choice.wanted_unready) {
        declined = !ensure_model_roms(nullptr, *launch.dirs, *choice.wanted_unready,
                                      "The profile \"" + start_name + "\"", choice.wanted_variant);
        choice = choose_start_model(wanted, roms);
        if (declined) {
            launch.skip_start_profile = true;
            start = Profile();
        }
    }
    // First start (or ROMs gone): the ROM manager, until one model can run.
    if (!choice.model && !declined && rom_manager_dialog(nullptr, *launch.dirs, nullptr, true))
        choice = choose_start_model(start_name.empty() ? std::string() : wanted, roms);
    if (!choice.model) {
        prepare_error("em34xx-qt needs the ROM images of at least one terminal model to start. Import them when you "
                      "start em34xx-qt again.");
        launch.exit_code = 1;
        return false;
    }
    const Model* model = choice.model;
    const ModelVariant& variant = choice.variant ? *choice.variant : model->default_variant();
    const std::string stored_as = model->storage_name(variant);

    // The global keymap, when there is one and it loads.
    launch.keymap_path.clear();
    std::error_code ec;
    if (std::filesystem::exists(launch.dirs->keymap_file(), ec)) {
        std::string text;
        KeymapFile file;
        if (read_text_file(launch.dirs->keymap_file(), text, error) &&
            parse_keymap_file(text, launch.dirs->keymap_file(), file, error))
            launch.keymap_path = launch.dirs->keymap_file();
        else
            error_box(nullptr, error + "\n\nThe default (PCOMM) keymap is used.");
    }

    launch.args = {"em34xx-qt",
                   "--model",
                   // The profile's variant when it can run, else the first
                   // variant that can.
                   variant.id,
                   "--rom-dir",
                   launch.dirs->roms(),
                   "--speed",
                   std::to_string(launch.settings.speed_percent),
                   "--keymap",
                   launch.keymap_path.empty() ? std::string("pcomm") : launch.keymap_path,
                   "--nvram",
                   launch.dirs->nvram_file(stored_as),
                   // The setup EEPROM (monitor alignment values) is kept too.
                   "--eeprom",
                   launch.dirs->eeprom_file(stored_as),
                   // An end-user build keeps running on an access the model does
                   // not decode (the default, stop, is for the debugger).
                   "--unmapped",
                   "log",
                   "--log",
                   log_spec(),
                   "--log-file",
                   launch.dirs->root() + "/em34xx-qt.log"};
    if (!start.crt) launch.args.push_back("--no-crt");
    if (start.model == model->info().name && !start.phosphor.empty() && variant.phosphor(start.phosphor)) {
        launch.args.push_back("--phosphor");
        launch.args.push_back(start.phosphor);
    }
    return true;
}

std::unique_ptr<Frontend> make_qt_frontend(QtLaunch& launch) { return std::make_unique<QtFrontend>(launch); }

int qt_run(Application& app, QtLaunch& launch) {
    std::vector<char*> argv;
    for (std::string& a : launch.args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    if (!app.start(int(launch.args.size()), argv.data())) {
        if (!app.last_error().empty()) error_box(nullptr, app.last_error());
        return app.exit_code();
    }
    return app.run();
}

void qt_fatal_box(const std::string& message) {
    std::fprintf(stderr, "em34xx-qt: %s\n", message.c_str());
    if (QApplication::instance()) error_box(nullptr, message);
}

} // namespace em34xx
