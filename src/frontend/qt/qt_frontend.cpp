// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/qt/qt_frontend.hpp"

#include "app/application.hpp"
#include "em34xx_version.h"
#include "frontend/qt/qt_keys.hpp"
#include "machine/emulator.hpp"
#include "present/display_profile.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QScreen>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <set>
#include <vector>

// No moc: nothing here declares signals or slots (no Q_OBJECT); connections
// are to lambdas and events are handled by overriding the virtuals.

namespace em34xx {

namespace {

using steady = std::chrono::steady_clock;

// The machine may report a change after every execution slice; fetch the
// raster (which holds the machine lock) at most once per refresh interval.
constexpr int kMinRasterIntervalMs = 15;
// Title, status bar, quit requests and a fallback look at the picture.
constexpr int kHousekeepingMs = 100;

// The terminal's screen: the tube image at the widget's real pixel size,
// centred on black, and the host keyboard while it has the focus.
class ScreenWidget : public QWidget {
public:
    ScreenWidget(Application& app, const CrtParams& crt, QWidget* parent)
        : QWidget(parent), app_(app), emu_(app.emulator()), crt_params_(crt) {
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMinimumSize(320, 240);
        raster_timer_.setSingleShot(true);
        QObject::connect(&raster_timer_, &QTimer::timeout, this, [this] { fetch(); });
        fetch_raster();
    }

    // The presented frame (aspect-corrected) in logical pixels.
    QSize presented_size() const { return QSize(fb_.presented_width(), fb_.presented_height()); }
    void set_size_hint(QSize s) {
        hint_ = s;
        updateGeometry();
    }
    QSize sizeHint() const override { return hint_.isValid() ? hint_ : presented_size(); }

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
        emu_.with_machine([&](Machine& m) {
            for (uint32_t hid : pressed_) {
                HostKeyEvent ev;
                ev.hid_usage = hid;
                ev.pressed = false;
                ev.name = qt::hid_key_name(hid);
                m.key_event(ev);
            }
        });
        pressed_.clear();
    }

protected:
    bool event(QEvent* e) override {
        switch (e->type()) {
        case QEvent::ShortcutOverride:
            // Every key belongs to the terminal while the screen has the
            // focus: no menu mnemonics (Alt), no shortcuts, no F10.
            e->accept();
            return true;
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
        const uint32_t hid = qt::hid_from_xkb_keycode(e->nativeScanCode());
        TN_LOG("input", Debug, "qt %s scan code %u -> HID %02X (%s) key %08X mod %08X%s", down ? "down" : "up  ",
               unsigned(e->nativeScanCode()), unsigned(hid), qt::hid_key_name(hid), unsigned(e->key()),
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
        ev.name = qt::hid_key_name(hid);
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
        const CrtRenderer::Rows rows = crt_.render(fb_, out_w, out_h, crt_params_, image_);
        if (resized) {
            update();
        } else if (!rows.empty()) {
            const int y0 = (px_h - out_h) / 2;
            const int top = int(std::floor((y0 + rows.row0) / dpr)) - 1;
            const int bottom = int(std::ceil((y0 + rows.row1) / dpr)) + 1;
            update(0, top, width(), bottom - top);
        }
    }

    Application& app_;
    Emulator& emu_;
    CrtParams crt_params_;
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

class QtFrontend : public Frontend {
public:
    const char* name() const override { return "qt"; }
    bool run(Application& app, const FrontendOptions& opts, std::string& error) override;
    void request_quit() override { quit_.store(true); }
    void set_title(const std::string& title) override {
        std::lock_guard<std::mutex> lock(text_mutex_);
        title_ = title;
        have_title_ = true;
    }
    void set_status(const std::string& status) override {
        std::lock_guard<std::mutex> lock(text_mutex_);
        status_ = status;
    }

private:
    std::atomic<bool> quit_{false};
    std::atomic<bool> wake_pending_{false};
    std::mutex text_mutex_;
    std::string title_;
    bool have_title_ = false;
    std::string status_;
};

QString qstr(const std::string& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }

// Press and release one of the terminal's own keys (Attn, SysRq, Reset,
// Setup), bypassing the host keymap.
void terminal_key(Emulator& emu, const char* function) {
    const bool ok = emu.with_machine([&](Machine& m) {
        if (!m.terminal_key(function, true)) return false;
        m.terminal_key(function, false);
        return true;
    });
    if (!ok) LOG_WARN("input", "this keyboard has no %s key", function);
}

bool QtFrontend::run(Application& app, const FrontendOptions& opts, std::string&) {
    Emulator& emu = app.emulator();

    // Qt reads its own options (-platform, -style...) from the command
    // line; the application has already parsed (and would have refused)
    // ours, so Qt gets the program name only.
    static char arg0[] = "em34xx-qt";
    static char* qt_argv[] = {arg0, nullptr};
    static int qt_argc = 1;
    QApplication::setApplicationName(QStringLiteral("em34xx"));
    QApplication::setApplicationVersion(QStringLiteral(EM34XX_VERSION_STRING));
    QApplication::setDesktopFileName(QStringLiteral("em34xx"));
    QApplication qapp(qt_argc, qt_argv);
    LOG_INFO("video", "Qt %s, platform %s, style %s", qVersion(), qPrintable(QGuiApplication::platformName()),
             qPrintable(QApplication::style()->objectName()));

    QMainWindow window;
    auto* screen = new ScreenWidget(app, opts.crt, &window);
    window.setCentralWidget(screen);

    // --- Status bar: the status note on the left, the run state on the right
    auto* note = new QLabel(&window);
    auto* run_state = new QLabel(&window);
    window.statusBar()->addWidget(note, 1);
    window.statusBar()->addPermanentWidget(run_state);

    // --- Menus (the layout of em34xxw's, as far as this front end goes) ----
    // The picture area becomes the presented frame times the zoom, shrunk
    // to fit the desktop; resizing by hand stays possible.
    auto apply_zoom = [&](double zoom) {
        QSize want = screen->presented_size() * zoom;
        if (const QScreen* s = window.screen() ? window.screen() : QGuiApplication::primaryScreen()) {
            const QSize avail = s->availableGeometry().size() * 0.9;
            if (want.width() > avail.width() || want.height() > avail.height())
                want.scale(avail, Qt::KeepAspectRatio);
        }
        if (window.isMaximized() || window.isFullScreen()) window.showNormal();
        screen->set_size_hint(want);
        window.resize(window.sizeHint());
    };
    // Menu commands run with the keys released and hand the keyboard back.
    auto add = [&](QMenu* menu, const QString& text, std::function<void()> fn) {
        QAction* a = menu->addAction(text);
        QObject::connect(a, &QAction::triggered, &window, [screen, fn = std::move(fn)] {
            screen->release_keys();
            fn();
            screen->setFocus();
        });
        return a;
    };

    QMenu* file = window.menuBar()->addMenu(QStringLiteral("&File"));
    add(file, QStringLiteral("E&xit"), [&] { window.close(); });

    QMenu* view = window.menuBar()->addMenu(QStringLiteral("&View"));
    QMenu* zoom_menu = view->addMenu(QStringLiteral("&Zoom"));
    auto* zoom_group = new QActionGroup(&window);
    const double initial_zoom = std::max(1, opts.scale);
    struct Zoom {
        const char* text;
        double factor;
    };
    for (const Zoom& z : {Zoom{"&1x", 1.0}, Zoom{"1&.5x", 1.5}, Zoom{"&2x", 2.0}}) {
        QAction* a = add(zoom_menu, QString::fromLatin1(z.text), [&apply_zoom, f = z.factor] { apply_zoom(f); });
        a->setCheckable(true);
        a->setChecked(z.factor == initial_zoom);
        zoom_group->addAction(a);
    }

    QMenu* terminal = window.menuBar()->addMenu(QStringLiteral("&Terminal"));
    add(terminal, QStringLiteral("&Attn"), [&] { terminal_key(emu, "attn"); });
    add(terminal, QStringLiteral("&SysRq"), [&] { terminal_key(emu, "sysrq"); });
    add(terminal, QStringLiteral("&Reset"), [&] { terminal_key(emu, "reset"); });
    add(terminal, QStringLiteral("Set&up"), [&] { terminal_key(emu, "setup"); });
    terminal->addSeparator();
    add(terminal, QStringLiteral("Re&boot (Power Cycle)"), [&] { app.power_on(); });

    QMenu* help = window.menuBar()->addMenu(QStringLiteral("&Help"));
    add(help, QStringLiteral("&About..."), [&] {
        QMessageBox::about(
            &window, QStringLiteral("About em34xx"),
            QStringLiteral("<b>em34xx %1</b><br>Hardware emulator for IBM 34xx-family 5250 terminals.<br><br>"
                           "<a href=\"%2\">%2</a><br><br>Qt %3 (%4)")
                .arg(QStringLiteral(EM34XX_VERSION_STRING), QStringLiteral(EM34XX_HOMEPAGE_URL),
                     QString::fromLatin1(qVersion()), QGuiApplication::platformName()));
    });

    // --- Picture: wake the UI thread when the emulator reports a change ----
    // (emulation thread; one queued call in flight at a time).
    emu.set_display_callback([this, screen](uint64_t) {
        if (wake_pending_.exchange(true)) return;
        QMetaObject::invokeMethod(
            screen,
            [this, screen] {
                wake_pending_.store(false);
                screen->picture_changed();
            },
            Qt::QueuedConnection);
    });
    struct ClearCallback {
        Emulator& emu;
        ~ClearCallback() { emu.set_display_callback(nullptr); }
    } clear_callback{emu};

    // --- Housekeeping: title, status bar, quit requests --------------------
    std::string shown_title, shown_note;
    int shown_running = -1;
    auto housekeeping = [&] {
        if (emu.quit_requested() || quit_.load()) {
            window.close();
            return;
        }
        std::string title, status;
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            title = have_title_ ? title_ : opts.title;
            status = status_;
        }
        if (title != shown_title) {
            window.setWindowTitle(qstr(title));
            shown_title = title;
        }
        if (status != shown_note) {
            note->setText(qstr(status));
            shown_note = status;
        }
        const int running = emu.running() ? 1 : 0;
        if (running != shown_running) {
            run_state->setText(running ? QStringLiteral("Running") : QStringLiteral("Stopped"));
            shown_running = running;
        }
        screen->poll_generation();
    };
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &window, housekeeping);
    timer.start(kHousekeepingMs);
    housekeeping();

    apply_zoom(initial_zoom);
    window.show();
    screen->setFocus();

    // Returns when the last window closes.
    QApplication::exec();
    if (!emu.quit_requested()) emu.request_quit(emu.exit_code());
    return true;
}

} // namespace

std::unique_ptr<Frontend> make_qt_frontend() { return std::make_unique<QtFrontend>(); }

} // namespace em34xx
