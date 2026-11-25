// SPDX-License-Identifier: GPL-3.0-or-later
// em34xx-qt driven end to end on Qt's offscreen platform: the real launch
// (qt_prepare), Application and window, with a script of steps played from a
// timer (which also runs inside modal dialogs). Each step waits for a window
// or dialog, checks it, optionally saves a screenshot, and acts on it as a
// user would (clicks a button by its text, fills a field).
//
//   em34xx_qt_gui_test SCENARIO ROM_DIR [SHOT_DIR]
//
// Scenarios: first-start, profiles, foreign, trust, burn-in (trust needs
// the openssl command; without it the test is skipped, exit code 77). ROM_DIR holds the ROM
// images (roms/, never committed); the test makes its own user folder
// (EM34XX_USER_DIR) in a temporary directory. SHOT_DIR, when given, gets a
// PNG of every screen a step names. See docs/frontend-qt.md, "Testing".
#include "app/application.hpp"
#include "frontend/qt/qt_frontend.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::string g_shots;
std::vector<pid_t> g_children;   // helper servers, ended at the end
int g_failures = 0;

void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

QString plain(QString s) { return s.remove(QLatin1Char('&')); }

// The visible top-level window titled `title` (a dialog, a message box or
// the main window); "" = the active modal widget.
QWidget* window_titled(const std::string& title) {
    if (title.empty()) return QApplication::activeModalWidget();
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->isVisible() && w->windowTitle() == QString::fromStdString(title)) return w;
    return nullptr;
}

QMainWindow* main_window() {
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* m = qobject_cast<QMainWindow*>(w); m && m->isVisible()) return m;
    return nullptr;
}

// A message box whose text contains `part`.
QMessageBox* message_box(const std::string& part) {
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* b = qobject_cast<QMessageBox*>(w); b && b->isVisible() && b->text().contains(QString::fromStdString(part)))
            return b;
    return nullptr;
}

bool click(QWidget* in, const std::string& text) {
    for (QAbstractButton* b : in->findChildren<QAbstractButton*>())
        if (plain(b->text()) == QString::fromStdString(text) && b->isVisible()) {
            if (!b->isEnabled()) {
                fail("button \"" + text + "\" is disabled");
                return false;
            }
            // Queued: a click that opens a modal dialog must not run inside
            // the script's tick (the dialog's own loop runs the next ticks).
            QMetaObject::invokeMethod(b, [b] { b->click(); }, Qt::QueuedConnection);
            return true;
        }
    fail("no button \"" + text + "\" in \"" + in->windowTitle().toStdString() + "\"");
    return false;
}

// A menu command of the main window by its menu and item text.
bool trigger(const std::string& menu, const std::string& item) {
    QMainWindow* w = main_window();
    if (!w) return false;
    for (QAction* m : w->menuBar()->actions()) {
        if (plain(m->text()) != QString::fromStdString(menu)) continue;
        std::vector<QMenu*> menus{m->menu()};
        while (!menus.empty()) {
            QMenu* cur = menus.back();
            menus.pop_back();
            Q_EMIT cur->aboutToShow();
            for (QAction* a : cur->actions()) {
                if (a->menu()) menus.push_back(a->menu());
                else if (plain(a->text()) == QString::fromStdString(item)) {
                    QMetaObject::invokeMethod(a, [a] { a->trigger(); }, Qt::QueuedConnection);
                    return true;
                }
            }
        }
    }
    fail("no menu item " + menu + " > " + item);
    return false;
}

// A message box's button, queued like click().
void press(QMessageBox* b, QMessageBox::StandardButton which) {
    QAbstractButton* button = b->button(which);
    QMetaObject::invokeMethod(button, [button] { button->click(); }, Qt::QueuedConnection);
}

void shot(QWidget* w, const std::string& name) {
    if (g_shots.empty() || !w) return;
    const std::string path = g_shots + "/" + name + ".png";
    if (!w->grab().save(QString::fromStdString(path))) fail("cannot save " + path);
}

QLineEdit* edit_after_label(QWidget* in, int index) {
    const QList<QLineEdit*> edits = in->findChildren<QLineEdit*>();
    return index < edits.size() ? edits[index] : nullptr;
}

// --- the script ---------------------------------------------------------------

struct Step {
    std::string what;
    std::function<bool()> run;   // true: done, go to the next step
    int timeout_ms = 20000;
};

class Script {
public:
    explicit Script(std::vector<Step> steps) : steps_(std::move(steps)) {}

    void start() {
        started_ = std::chrono::steady_clock::now();
        QObject::connect(&timer_, &QTimer::timeout, [this] { tick(); });
        timer_.start(100);
    }
    bool finished() const { return next_ >= steps_.size(); }

private:
    void tick() {
        if (busy_ || finished()) return;
        Step& s = steps_[next_];
        busy_ = true;   // a step's click may open a modal dialog: ticks run inside it
        const bool done = s.run();
        busy_ = false;
        const auto now = std::chrono::steady_clock::now();
        if (done) {
            std::fprintf(stderr, "ok: %s\n", s.what.c_str());
            ++next_;
            started_ = now;
            return;
        }
        if (now - started_ > std::chrono::milliseconds(s.timeout_ms)) {
            fail("timed out: " + s.what);
            // Leave without questions.
            next_ = steps_.size();
            std::raise(SIGTERM);
        }
    }

    std::vector<Step> steps_;
    size_t next_ = 0;
    bool busy_ = false;
    QTimer timer_;
    std::chrono::steady_clock::time_point started_;
};

// Waits `ms` (counted from the first call).
std::function<bool()> wait_ms(int ms) {
    auto t0 = std::make_shared<std::chrono::steady_clock::time_point>();
    return [t0, ms] {
        const auto now = std::chrono::steady_clock::now();
        if (*t0 == std::chrono::steady_clock::time_point()) *t0 = now;
        return now - *t0 >= std::chrono::milliseconds(ms);
    };
}

void copy_roms(const std::string& from, const fs::path& to, const std::vector<std::string>& files) {
    fs::create_directories(to);
    for (const std::string& f : files) fs::copy_file(fs::path(from) / f, to / f, fs::copy_options::overwrite_existing);
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string status_text() {
    QMainWindow* w = main_window();
    if (!w) return {};
    for (QLabel* l : w->statusBar()->findChildren<QLabel*>())
        if (l->text().startsWith(QStringLiteral("Profile:")) || l->text() == QStringLiteral("no profile"))
            return l->text().toStdString();
    return {};
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s SCENARIO ROM_DIR [SHOT_DIR]\n", argv[0]);
        return 2;
    }
    const std::string scenario = argv[1], roms = argv[2];
    if (argc > 3) {
        g_shots = argv[3];
        fs::create_directories(g_shots);
    }
    const fs::path user = fs::temp_directory_path() / ("em34xx-qt-test-" + std::to_string(::getpid()));
    fs::remove_all(user);
    fs::create_directories(user);
    ::setenv("EM34XX_USER_DIR", user.c_str(), 1);
    // The file association writes below XDG_DATA_HOME and XDG_CONFIG_HOME:
    // never the user's own.
    ::setenv("XDG_DATA_HOME", (user / "xdg-data").c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (user / "xdg-config").c_str(), 1);
    ::setenv("QT_QPA_PLATFORM", "offscreen", 1);
    const std::vector<std::string> rom3477 = {"95f4171.bin", "95f4172.bin"};

    std::vector<Step> steps;
    std::vector<std::string> args = {"em34xx-qt"};
    int want_exit = 0;
    std::function<void()> after;   // checks once the program has ended

    if (scenario == "first-start") {
        // No ROM images: the ROM manager opens before any window and says one
        // model's images are needed; closing it with none ends the program.
        want_exit = 1;
        steps = {
            {"ROM dialog at first start",
             [] {
                 QWidget* d = window_titled("ROM Images");
                 if (!d) return false;
                 bool says = false;
                 for (QLabel* l : d->findChildren<QLabel*>())
                     says = says || l->text().contains(QStringLiteral("No terminal can run yet"));
                 if (!says) fail("the first-start ROM dialog does not ask for one model's images");
                 shot(d, "first-start-roms");
                 return click(d, "Close");
             }},
            {"error box: ROM images needed",
             [] {
                 QMessageBox* b = message_box("needs the ROM images");
                 if (!b) return false;
                 shot(b, "first-start-error");
                 b->accept();
                 return true;
             }},
        };
    } else if (scenario == "profiles") {
        copy_roms(roms, user / "roms", rom3477);
        steps = {
            {"profile manager at start",
             [] {
                 QWidget* d = window_titled("Connect to Profile");
                 if (!d || !main_window()) return false;
                 shot(main_window(), "main-idle");
                 shot(d, "profile-manager-empty");
                 return click(d, "New...");
             }},
            {"name prompt",
             [] {
                 QWidget* d = window_titled("New Profile");
                 if (!d) return false;
                 edit_after_label(d, 0)->setText(QStringLiteral("Local test"));
                 shot(d, "name-prompt");
                 return click(d, "OK");
             }},
            {"profile editor",
             [] {
                 QWidget* d = window_titled("New Profile: Local test");
                 if (!d) return false;
                 // Host, port, device of session 1 (the line edits in order).
                 const QList<QLineEdit*> e = d->findChildren<QLineEdit*>();
                 if (e.size() < 6) {
                     fail("the profile editor has too few fields");
                     return true;
                 }
                 e[0]->setText(QStringLiteral("127.0.0.1"));
                 e[1]->setText(QStringLiteral("9"));   // discard: nothing listens, the session retries
                 // Use TLS on session 1 swaps 23 and 992 only; 9 stays.
                 shot(d, "profile-editor");
                 return click(d, "Save");
             }},
            {"profile listed, edit TLS settings",
             [] {
                 QWidget* d = window_titled("Connect to Profile");
                 if (!d) return false;
                 auto* list = d->findChild<QListWidget*>();
                 if (!list || list->count() != 1 || list->item(0)->text() != QStringLiteral("Local test"))
                     fail("the new profile is not listed");
                 shot(d, "profile-manager");
                 return click(d, "Copy");
             }},
            {"copy listed",
             [] {
                 QWidget* d = window_titled("Connect to Profile");
                 auto* list = d ? d->findChild<QListWidget*>() : nullptr;
                 if (!list || list->count() != 2) return false;
                 if (list->item(1)->text() != QStringLiteral("Local test (copy)")) fail("copy has the wrong name");
                 return click(d, "Delete");
             }},
            {"delete asks",
             [] {
                 QMessageBox* b = message_box("Delete the profile");
                 if (!b) return false;
                 press(b, QMessageBox::Yes);
                 return true;
             }},
            {"connect",
             [] {
                 QWidget* d = window_titled("Connect to Profile");
                 auto* list = d ? d->findChild<QListWidget*>() : nullptr;
                 if (!list || list->count() != 1) return false;
                 list->setCurrentRow(0);
                 return click(d, "Connect");
             }},
            {"connected: title and status bar",
             [] {
                 QMainWindow* w = main_window();
                 if (!w || QApplication::activeModalWidget()) return false;
                 if (!w->windowTitle().startsWith(QStringLiteral("Local test - IBM 3477"))) return false;
                 if (status_text() != "Profile: Local test") fail("status bar: " + status_text());
                 return true;
             }},
            {"POST runs", wait_ms(4000)},
            {"main window shot",
             [] {
                 shot(main_window(), "main-connected");
                 return trigger("Edit", "Connection Settings...");
             }},
            {"connection settings",
             [] {
                 QWidget* d = window_titled("Connection Settings: Local test");
                 if (!d) return false;
                 const QList<QLineEdit*> e = d->findChildren<QLineEdit*>();
                 e[3]->setText(QStringLiteral("127.0.0.2"));   // session 2 host
                 shot(d, "connection-settings");
                 return click(d, "OK");
             }},
            {"modified: asterisk",
             [] {
                 if (QApplication::activeModalWidget()) return false;
                 if (status_text() != "Profile: Local test*") fail("not marked modified: " + status_text());
                 return trigger("File", "Save");
             }},
            {"saved",
             [] {
                 if (status_text() != "Profile: Local test") return false;
                 return trigger("Edit", "Settings...");
             }},
            {"settings",
             [] {
                 QWidget* d = window_titled("Settings");
                 if (!d) return false;
                 shot(d, "settings");
                 return click(d, "Cancel");
             }},
            {"keymap", [] { return trigger("Edit", "Keymap..."); }},
            {"keymap dialog",
             [] {
                 QWidget* d = window_titled("Keymap");
                 if (!d) return false;
                 auto* list = d->findChild<QTreeWidget*>();
                 if (!list || list->topLevelItemCount() < 10) fail("the keymap lists too few entries");
                 shot(d, "keymap");
                 return click(d, "Add...");
             }},
            {"key mapping: capture Ctrl+F5",
             [] {
                 QWidget* d = window_titled("Key Mapping");
                 if (!d) return false;
                 // The capture box: the first focusable frame. F5 is evdev
                 // code 63, XKB keycode 71.
                 QWidget* capture = d->findChild<QWidget*>(QStringLiteral("capture"));
                 if (!capture) {
                     fail("no key capture box");
                     return true;
                 }
                 QKeyEvent press(QEvent::KeyPress, Qt::Key_F5, Qt::ControlModifier, 71, 0, 0);
                 QApplication::sendEvent(capture, &press);
                 auto* host = d->findChildren<QComboBox*>().value(0);
                 if (!host || host->currentText() != QStringLiteral("F5"))
                     fail("captured key: " + (host ? host->currentText().toStdString() : std::string("?")));
                 bool ctrl = false;
                 for (QCheckBox* c : d->findChildren<QCheckBox*>())
                     if (plain(c->text()) == QStringLiteral("Ctrl")) ctrl = c->isChecked();
                 if (!ctrl) fail("Ctrl was not captured");
                 shot(d, "key-mapping");
                 return click(d, "Cancel");
             }},
            {"keymap OK writes keymap.toml",
             [] {
                 QWidget* d = window_titled("Keymap");
                 return d && click(d, "OK");
             }},
            {"about", [] { return trigger("Help", "About..."); }},
            {"about dialog",
             [] {
                 QWidget* d = window_titled("About em34xx");
                 if (!d) return false;
                 bool notices = false;
                 for (QPlainTextEdit* t : d->findChildren<QPlainTextEdit*>())
                     notices = notices || t->toPlainText().contains(QStringLiteral("toml++"));
                 if (!notices) fail("About shows no third-party notices");
                 shot(d, "about");
                 return click(d, "OK");
             }},
            {"roms", [] { return trigger("Edit", "Settings..."); }},
            {"settings > ROM images",
             [] {
                 QWidget* d = window_titled("Settings");
                 return d && click(d, "ROM Images...");
             }},
            {"ROM dialog",
             [] {
                 QWidget* d = window_titled("ROM Images");
                 if (!d) return false;
                 shot(d, "roms");
                 click(d, "Close");
                 return true;
             }},
            {"settings > file association",
             [] {
                 QWidget* d = window_titled("Settings");
                 return d && !QApplication::activeModalWidget()->inherits("QMessageBox") &&
                        QApplication::activeModalWidget() == d && click(d, "Open .em34xx files with em34xx-qt...");
             }},
            {"file association asks",
             [] {
                 QMessageBox* b = message_box("Open .em34xx profile files");
                 if (!b) return false;
                 press(b, QMessageBox::Yes);
                 return true;
             }},
            {"file association done",
             [] {
                 QMessageBox* b = message_box(".em34xx files now open");
                 if (!b) return false;
                 press(b, QMessageBox::Ok);
                 return true;
             }},
            {"settings closed",
             [] {
                 QWidget* d = window_titled("Settings");
                 return d && QApplication::activeModalWidget() == d && click(d, "Cancel");
             }},
            {"zoom 2x", [] { return trigger("View", "2x"); }},
            {"zoom shot",
             [] {
                 shot(main_window(), "main-zoom2");
                 return trigger("View", "1x");
             }},
            {"disconnect", [] { return trigger("File", "Disconnect"); }},
            {"reconnect offered",
             [] {
                 QMainWindow* w = main_window();
                 for (QAction* a : w->findChild<QToolBar*>()->actions())
                     if (a->toolTip() == QStringLiteral("Reconnect")) return true;
                 return false;
             }},
            {"exit", [] { return trigger("File", "Exit"); }},
        };
        after = [user] {
            if (!fs::exists(user / "profiles" / "Local test.em34xx")) fail("the profile was not saved");
            else if (read_file(user / "profiles" / "Local test.em34xx").find("127.0.0.2") == std::string::npos)
                fail("the saved profile lacks the edited session 2");
            if (fs::exists(user / "profiles" / "Local test (copy).em34xx")) fail("the deleted copy is still there");
            if (!fs::exists(user / "3477.nvram")) fail("no 3477.nvram written");
            const std::string desktop = read_file(user / "xdg-data" / "applications" / "em34xx.desktop");
            if (desktop.find("MimeType=application/x-em34xx-profile;") == std::string::npos ||
                desktop.find("em34xx_qt_gui_test\" %f") == std::string::npos)
                fail("the desktop entry does not open profiles with this executable:\n" + desktop);
            if (!fs::exists(user / "xdg-data" / "mime" / "packages" / "em34xx.xml")) fail("no file type written");
            if (!fs::exists(user / "keymap.toml")) fail("no keymap.toml written");
            if (read_file(user / "settings.toml").find("[window]") == std::string::npos)
                fail("settings.toml has no window placement");
        };
    } else if (scenario == "foreign") {
        // A profile from outside the profiles folder is asked about first;
        // No leaves the terminal idle.
        copy_roms(roms, user / "roms", rom3477);
        const fs::path outside = user / "elsewhere.em34xx";
        std::ofstream(outside) << "format = 3\nmodel = \"3477\"\n[tls]\nverify = \"none\"\n[[session]]\n"
                                  "host = \"evil.example\"\nport = 992\ntls = true\n";
        args.push_back(outside.string());
        steps = {
            {"question about the foreign profile",
             [] {
                 QMessageBox* b = message_box("not in your profiles folder");
                 if (!b) return false;
                 if (!b->text().contains(QStringLiteral("evil.example:992 - TLS, certificate NOT VERIFIED")))
                     fail("the question does not say how the certificate is checked");
                 if (b->defaultButton() != b->button(QMessageBox::No)) fail("No is not the default");
                 shot(b, "foreign-profile");
                 press(b, QMessageBox::No);
                 return true;
             }},
            {"idle, no profile",
             [] {
                 if (QApplication::activeModalWidget()) return false;
                 if (status_text() != "no profile") fail("status bar: " + status_text());
                 return trigger("File", "Exit");
             }},
        };
    } else if (scenario == "trust") {
        // Two local TLS servers: a self-signed certificate (the trust
        // prompt: Trust Permanently pins it in the profile) and an expired
        // one (the invalid-certificate prompt: Cancel holds the session).
        copy_roms(roms, user / "roms", rom3477);
        const fs::path pki = user / "pki";
        fs::create_directories(pki);
        const std::string q = "'" + pki.string() + "/";
        const std::string subj = " -subj /CN=em34xx-test -addext subjectAltName=IP:127.0.0.1 2>/dev/null";
        if (std::system(("openssl req -x509 -newkey rsa:2048 -nodes -days 30 -keyout " + q + "self.key' -out " + q +
                         "self.pem'" + subj)
                            .c_str()) != 0 ||
            std::system(("openssl req -x509 -newkey rsa:2048 -nodes -not_before 20200101000000Z -not_after "
                         "20210101000000Z -keyout " +
                         q + "old.key' -out " + q + "old.pem'" + subj)
                            .c_str()) != 0) {
            std::fprintf(stderr, "SKIP: openssl cannot make the test certificates\n");
            return 77;
        }
        const int port_self = 20000 + int(::getpid() % 20000), port_old = port_self + 1;
        for (auto [port, name] : {std::pair<int, const char*>{port_self, "self"}, {port_old, "old"}}) {
            const pid_t pid = ::fork();
            if (pid == 0) {
                const std::string accept = "127.0.0.1:" + std::to_string(port);
                const std::string cert = (pki / (std::string(name) + ".pem")).string();
                const std::string key = (pki / (std::string(name) + ".key")).string();
                ::execlp("openssl", "openssl", "s_server", "-quiet", "-accept", accept.c_str(), "-cert", cert.c_str(),
                         "-key", key.c_str(), static_cast<char*>(nullptr));
                ::_exit(127);
            }
            g_children.push_back(pid);
        }
        ::usleep(500000);   // the servers listen
        fs::create_directories(user / "profiles");
        auto profile = [&](const char* name, int port) {
            std::ofstream(user / "profiles" / (std::string(name) + ".em34xx"))
                << "format = 3\nmodel = \"3477\"\n[[session]]\nhost = \"127.0.0.1\"\nport = " << port
                << "\ntls = true\n";
        };
        profile("Self-signed", port_self);
        profile("Expired", port_old);
        args.push_back((user / "profiles" / "Self-signed.em34xx").string());
        steps = {
            {"Untrusted Certificate",
             [] {
                 QWidget* d = window_titled("Untrusted Certificate");
                 if (!d || QApplication::activeModalWidget() != d) return false;
                 bool says = false;
                 for (QLabel* l : d->findChildren<QLabel*>()) says = says || l->text().contains(QStringLiteral("is not trusted"));
                 if (!says) fail("the trust prompt does not say the certificate is not trusted");
                 shot(d, "trust-prompt");
                 shot(main_window(), "main-trust-waiting");
                 return click(d, "Trust Permanently");
             }},
            {"pinned in the profile",
             [user] {
                 const std::string p = read_file(user / "profiles" / "Self-signed.em34xx");
                 return p.find("fingerprint") != std::string::npos && p.find("cert_sha256") != std::string::npos;
             }},
            {"connect the expired one", [] { return trigger("File", "Connect to Profile..."); }},
            {"choose Expired",
             [] {
                 QWidget* d = window_titled("Connect to Profile");
                 auto* list = d ? d->findChild<QListWidget*>() : nullptr;
                 if (!list) return false;
                 for (int i = 0; i < list->count(); ++i)
                     if (list->item(i)->text() == QStringLiteral("Expired")) list->setCurrentRow(i);
                 return click(d, "Connect");
             }},
            {"Certificate Not Valid",
             [] {
                 QWidget* d = window_titled("Certificate Not Valid");
                 if (!d || QApplication::activeModalWidget() != d) return false;
                 for (QAbstractButton* b : d->findChildren<QAbstractButton*>())
                     if (plain(b->text()) == QStringLiteral("Connect Anyway") && b->isEnabled())
                         fail("Connect Anyway is enabled before the risk is acknowledged");
                 shot(d, "certificate-invalid");
                 return click(d, "Cancel");
             }},
            {"held, no second prompt", wait_ms(3000)},
            {"exit",
             [] {
                 if (window_titled("Certificate Not Valid")) fail("the cancelled session asked again");
                 return trigger("File", "Exit");
             }},
        };
    } else if (scenario == "burn-in") {
        copy_roms(roms, user / "roms", rom3477);
        std::ofstream(user / "settings.toml") << "[display]\nburn_in = \"hyper\"\n";
        steps = {
            {"profile manager", [] {
                 QWidget* d = window_titled("Connect to Profile");
                 return d && click(d, "Close");
             }},
            {"tube wears", wait_ms(6000)},
            {"replace CRT", [] { return trigger("View", "Replace CRT..."); }},
            {"replace asks",
             [] {
                 QMessageBox* b = message_box("Replace the CRT");
                 if (!b) return false;
                 press(b, QMessageBox::Yes);
                 return true;
             }},
            {"burn-in off", [] { return trigger("View", "Off"); }},
            {"exit", [] { return trigger("File", "Exit"); }},
        };
        after = [user] {
            if (!fs::exists(user / "tube.burn")) fail("no tube.burn written");
            const std::string settings = read_file(user / "settings.toml");
            if (settings.find("burn_in = \"off\"") == std::string::npos &&
                settings.find("burn_in = 'off'") == std::string::npos)
                fail("View > Burn-in > Off was not saved:\n" + settings);
        };
    } else {
        std::fprintf(stderr, "unknown scenario %s\n", scenario.c_str());
        return 2;
    }

    std::vector<char*> argv2;
    for (std::string& a : args) argv2.push_back(&a[0]);
    argv2.push_back(nullptr);
    int code = 0;
    bool finished = false;
    {
        em34xx::QtLaunch launch;
        Script script(steps);   // goes before the QApplication
        // The script runs during qt_prepare too (the first-start ROM dialog):
        // it starts as soon as the QApplication exists.
        launch.on_qapp = [&] { script.start(); };
        if (!em34xx::qt_prepare(int(args.size()), argv2.data(), launch)) {
            code = launch.exit_code;
        } else {
            em34xx::Application app(em34xx::make_qt_frontend(launch), nullptr);
            code = em34xx::qt_run(app, launch);
        }
        finished = script.finished();
    }
    if (!finished) fail("the program ended before the script did");
    if (code != want_exit) fail("exit code " + std::to_string(code) + ", expected " + std::to_string(want_exit));
    for (pid_t pid : g_children) {
        ::kill(pid, SIGTERM);
        ::waitpid(pid, nullptr, 0);
    }
    if (after) after();
    fs::remove_all(user);
    std::fprintf(stderr, "%s: %s\n", scenario.c_str(), g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
