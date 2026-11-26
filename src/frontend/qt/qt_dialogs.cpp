// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/qt/qt_dialogs.hpp"

#include "em34xx_version.h"
#include "frontend/qt/qt_keys.hpp"
#include "frontend/qt/qt_util.hpp"
#include "models/model.hpp"
#include "tn5250/tls_policy.hpp"
#include "util/fingerprint.hpp"
#include "util/strings.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

namespace em34xx::qt {

namespace {

const unsigned kSpeeds[] = {25, 50, 100, 200, 400, 0};
const TlsVersion kTlsVersions[] = {TlsVersion::Ssl3, TlsVersion::Tls1_0, TlsVersion::Tls1_1, TlsVersion::Tls1_2,
                                   TlsVersion::Tls1_3};
constexpr int kTlsVersionCount = int(sizeof kTlsVersions / sizeof kTlsVersions[0]);

std::string tls_version_label(TlsVersion v) {
    switch (v) {
    case TlsVersion::Ssl3: return "SSL 3.0 (broken)";
    case TlsVersion::Tls1_0: return "TLS 1.0 (legacy)";
    case TlsVersion::Tls1_1: return "TLS 1.1 (legacy)";
    case TlsVersion::Tls1_2: return "TLS 1.2";
    case TlsVersion::Tls1_3: return "TLS 1.3";
    case TlsVersion::Any: break;
    }
    return "?";
}

std::string certificate_text(const tn5250::PeerCertificate& c) {
    std::string t;
    t += "Subject:\t" + c.subject + "\n";
    t += "Issuer:\t" + c.issuer + "\n";
    t += "Valid from:\t" + (c.valid_from.empty() ? std::string("?") : c.valid_from) + "\n";
    t += "Valid until:\t" + c.expires + (c.expired ? "  (EXPIRED)" : "") + "\n";
    t += "SHA-256:\t" + (c.have_digest ? format_sha256_fingerprint(c.sha256) : std::string("unavailable")) + "\n";
    t += c.chain_ok ? std::string("Verification:\tOK\n") : "Verification:\tfailed: " + c.chain_error + "\n";
    // Validity period and revocation, apart from trust: a pin does not
    // settle these.
    for (const std::string& e : tn5250::tls_validity_errors(c)) t += "NOT VALID:\t" + e + "\n";
    if (!c.revoked)
        t += c.revocation_checked ? std::string("Revocation:\tnot revoked\n")
                                  : "Revocation:\tnot checked" +
                                        (c.revocation_note.empty() ? std::string() : " (" + c.revocation_note + ")") +
                                        "\n";
    return t;
}

// A read-only box for certificate details: tab stops line up the values.
QPlainTextEdit* details_box(QWidget* parent, const std::string& text, int lines) {
    auto* box = new QPlainTextEdit(parent);
    box->setReadOnly(true);
    box->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    box->setTabStopDistance(box->fontMetrics().horizontalAdvance(QStringLiteral("Verification: ")) + 8);
    box->setPlainText(qs(text));
    box->setMinimumHeight(box->fontMetrics().lineSpacing() * lines + 12);
    box->setMinimumWidth(box->fontMetrics().averageCharWidth() * 72);
    return box;
}

QDialog* make_dialog(QWidget* owner, const std::string& title) {
    auto* d = new QDialog(owner);
    d->setWindowTitle(qs(title));
    d->setAttribute(Qt::WA_DeleteOnClose, false);
    return d;
}

// The dialogs are created on the stack-like pattern: a unique_ptr owns the
// QDialog (its children go with it), exec() runs it.
using DialogPtr = std::unique_ptr<QDialog>;

QPushButton* add_button(QDialogButtonBox* box, const char* text, QDialogButtonBox::ButtonRole role) {
    return box->addButton(qs(text), role);
}

} // namespace

// ---------------------------------------------------------------------------
// ROM images

bool rom_manager_dialog(QWidget* owner, const UserDirs& dirs, const Model* required, bool need_any,
                        const ModelVariant* required_variant) {
    DialogPtr dlg(make_dialog(owner, "ROM Images"));
    auto* layout = new QVBoxLayout(dlg.get());
    auto* intro = plain_label(dlg.get());
    intro->setWordWrap(true);
    if (required && !model_roms_ready(*required, dirs.roms(), required_variant))
        intro->setText(QStringLiteral(
            "The ROM images the terminal needs are missing or damaged. Import them to continue."));
    else if (need_any && ready_models(dirs.roms()).empty())
        intro->setText(QStringLiteral("No terminal can run yet. Import the ROM images of at least one terminal model "
                                      "to continue; the others can follow later."));
    else
        intro->setText(QStringLiteral("The terminals run their own firmware, which em34xx does not include. Import "
                                      "your ROM dumps here; they are recognised by their SHA-256."));
    layout->addWidget(intro);

    auto* list = new QTreeWidget(dlg.get());
    list->setColumnCount(4);
    list->setHeaderLabels({QStringLiteral("Model"), QStringLiteral("File"), QStringLiteral("ROM"),
                           QStringLiteral("Status")});
    list->setRootIsDecorated(false);
    list->setAlternatingRowColors(true);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setMinimumSize(720, 220);
    layout->addWidget(list);
    auto* folder = plain_label(qs("ROM folder: " + dirs.roms()), dlg.get());
    folder->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(folder);

    auto* buttons = new QDialogButtonBox(dlg.get());
    QPushButton* import = add_button(buttons, "&Import...", QDialogButtonBox::ActionRole);
    QPushButton* remove = add_button(buttons, "&Remove", QDialogButtonBox::ActionRole);
    QPushButton* recheck = add_button(buttons, "Re-&check", QDialogButtonBox::ActionRole);
    QPushButton* show = add_button(buttons, "Show &Folder", QDialogButtonBox::ActionRole);
    QPushButton* close = buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    std::vector<RomStatusRow> rows;
    auto fill = [&] {
        const int keep = list->indexOfTopLevelItem(list->currentItem());
        list->clear();
        rows = rom_status(dirs.roms());
        if (required) {
            std::stable_sort(rows.begin(), rows.end(), [&](const RomStatusRow& a, const RomStatusRow& b) {
                return (a.model == required->info().name) > (b.model == required->info().name);
            });
        }
        for (const RomStatusRow& r : rows) {
            std::string state = rom_file_state_name(r.status.state);
            if (!r.status.note.empty()) state += " (" + r.status.note + ")";
            auto* item = new QTreeWidgetItem(list);
            item->setText(0, qs("IBM " + r.model + (r.model_status == "bring-up" ? "" : " [" + r.model_status + "]")));
            item->setText(1, qs(r.spec.file));
            item->setText(2, qs(r.spec.label));
            item->setText(3, qs(state));
        }
        for (int c = 0; c < 4; ++c) list->resizeColumnToContents(c);
        if (list->topLevelItemCount() > 0)
            list->setCurrentItem(list->topLevelItem(std::clamp(keep, 0, list->topLevelItemCount() - 1)));
    };
    fill();

    QObject::connect(import, &QPushButton::clicked, dlg.get(), [&] {
        guarded(dlg.get(), [&] {
            const QStringList files = QFileDialog::getOpenFileNames(
                dlg.get(), QStringLiteral("Import ROM Images"), QDir::homePath(),
                QStringLiteral("ROM images (*.bin *.rom *.BIN *.ROM);;All files (*)"));
            std::string report;
            for (const QString& f : files) {
                std::string stored, note, error;
                if (dirs.import_rom(us(f), stored, note, error)) report += stored + ": " + note + "\n";
                else report += error + "\n";
            }
            fill();
            if (!report.empty()) info_box(dlg.get(), report);
        });
    });
    QObject::connect(remove, &QPushButton::clicked, dlg.get(), [&] {
        guarded(dlg.get(), [&] {
            const int sel = list->indexOfTopLevelItem(list->currentItem());
            if (sel < 0 || size_t(sel) >= rows.size()) return;
            const RomStatusRow r = rows[size_t(sel)];
            if (r.status.state == RomFileStatus::State::Missing) return;
            if (!ask_yes_no(dlg.get(), "Remove " + r.spec.file + " from the ROM folder?")) return;
            std::string error;
            if (!dirs.remove_rom(r.spec.file, error)) error_box(dlg.get(), error);
            fill();
        });
    });
    QObject::connect(recheck, &QPushButton::clicked, dlg.get(), [&] { fill(); });
    QObject::connect(show, &QPushButton::clicked, dlg.get(), [&] { open_with_desktop(dlg.get(), dirs.roms()); });
    QObject::connect(close, &QPushButton::clicked, dlg.get(), &QDialog::accept);
    (required || need_any ? static_cast<QWidget*>(import) : list)->setFocus();
    dlg->exec();
    if (required) return model_roms_ready(*required, dirs.roms(), required_variant);
    return !need_any || !ready_models(dirs.roms()).empty();
}

bool ensure_model_roms(QWidget* owner, const UserDirs& dirs, const Model& model, const std::string& what,
                       const ModelVariant* variant) {
    if (model_roms_ready(model, dirs.roms(), variant)) return true;
    const std::string name = "IBM " + (variant ? variant->id : model.info().name);
    const std::string text = (what.empty() ? "The " + name + " needs" : what + " is for the " + name + ", which needs") +
                             " ROM images that are not in the ROM folder:\n\n" +
                             unusable_roms_text(model, dirs.roms(), variant) +
                             "\nThe emulator runs the terminal's own firmware and does not include it. "
                             "Import the missing images now?";
    if (!ask_yes_no(owner, text, true)) return false;
    return rom_manager_dialog(owner, dirs, &model, false, variant);
}

// ---------------------------------------------------------------------------
// Settings

bool settings_dialog(QWidget* owner, const UserDirs& dirs, AppSettings& s) {
    DialogPtr dlg(make_dialog(owner, "Settings"));
    auto* layout = new QVBoxLayout(dlg.get());

    auto* startup = new QGroupBox(QStringLiteral("Startup"), dlg.get());
    auto* sl = new QHBoxLayout(startup);
    auto* open = new QCheckBox(QStringLiteral("&Open session on startup:"), startup);
    auto* profile = new QComboBox(startup);
    profile->setMinimumContentsLength(24);
    sl->addWidget(open);
    sl->addWidget(profile, 1);
    layout->addWidget(startup);
    open->setChecked(s.startup_open);
    const std::vector<std::string> profiles = dirs.list_profiles();
    profile->addItem(QString());
    for (size_t i = 0; i < profiles.size(); ++i) {
        profile->addItem(qs(profiles[i]));
        if (profiles[i] == s.startup_profile) profile->setCurrentIndex(int(i + 1));
    }
    profile->setEnabled(open->isChecked());
    QObject::connect(open, &QCheckBox::toggled, profile, &QWidget::setEnabled);

    auto* emulation = new QGroupBox(QStringLiteral("Emulation"), dlg.get());
    auto* form = new QFormLayout(emulation);
    auto* speed = new QComboBox(emulation);
    int speed_sel = 2;
    for (size_t i = 0; i < sizeof kSpeeds / sizeof kSpeeds[0]; ++i) {
        speed->addItem(kSpeeds[i] ? qs(format("%u %%", kSpeeds[i])) : QStringLiteral("Unthrottled"));
        if (kSpeeds[i] == s.speed_percent) speed_sel = int(i);
    }
    speed->setCurrentIndex(speed_sel);
    form->addRow(QStringLiteral("&Speed:"), speed);
    auto* tls = new QComboBox(emulation);
    for (int i = 0; i < kTlsVersionCount; ++i) {
        tls->addItem(qs(tls_version_label(kTlsVersions[i])));
        if (kTlsVersions[i] == s.tls_min_version) tls->setCurrentIndex(i);
    }
    form->addRow(QStringLiteral("Default minimum &TLS version:"), tls);
    auto* burn = new QComboBox(emulation);
    for (const char* label : {"Off", "Slow", "Fast", "Hyper"}) burn->addItem(qs(label));
    burn->setCurrentIndex(int(s.burn_in));
    form->addRow(QStringLiteral("Screen &burn-in:"), burn);
    layout->addWidget(emulation);

    auto* files = new QGroupBox(QStringLiteral("Files"), dlg.get());
    auto* fl = new QVBoxLayout(files);
    auto* where = plain_label(qs("Settings, ROMs, keymap and profiles are kept in " + dirs.root()), files);
    where->setWordWrap(true);
    where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fl->addWidget(where);
    auto* row = new QHBoxLayout;
    auto* roms = new QPushButton(QStringLiteral("&ROM Images..."), files);
    auto* folder = new QPushButton(QStringLiteral("Open User &Folder"), files);
    row->addWidget(roms);
    row->addWidget(folder);
    row->addStretch(1);
    fl->addLayout(row);
    auto* assoc_row = new QHBoxLayout;
    auto* assoc = new QPushButton(QStringLiteral("Open .em34xx files with em34xx-&qt..."), files);
    assoc_row->addWidget(assoc);
    assoc_row->addStretch(1);
    fl->addLayout(assoc_row);
    layout->addWidget(files);

    QObject::connect(roms, &QPushButton::clicked, dlg.get(),
                     [&] { guarded(dlg.get(), [&] { rom_manager_dialog(dlg.get(), dirs, nullptr); }); });
    QObject::connect(folder, &QPushButton::clicked, dlg.get(), [&] { open_with_desktop(dlg.get(), dirs.root()); });
    QObject::connect(assoc, &QPushButton::clicked, dlg.get(), [&] {
        guarded(dlg.get(), [&] {
            if (!ask_yes_no(dlg.get(),
                            "Open .em34xx profile files with this copy of em34xx-qt when you open them in the file "
                            "manager?\n\nThis writes a file type and a desktop entry for your user account only "
                            "(in " +
                                us(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)) + ")."))
                return;
            std::string error;
            if (register_file_association(error)) info_box(dlg.get(), ".em34xx files now open with em34xx-qt.");
            else error_box(dlg.get(), error);
        });
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg.get());
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dlg.get(), &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg.get(), &QDialog::reject);
    if (dlg->exec() != QDialog::Accepted) return false;

    s.startup_open = open->isChecked();
    const int pr = profile->currentIndex();
    s.startup_profile = pr > 0 && size_t(pr) <= profiles.size() ? profiles[size_t(pr - 1)] : "";
    if (speed->currentIndex() >= 0) s.speed_percent = kSpeeds[speed->currentIndex()];
    if (tls->currentIndex() >= 0) s.tls_min_version = kTlsVersions[tls->currentIndex()];
    if (burn->currentIndex() >= 0) s.burn_in = BurnSpeed(burn->currentIndex());
    return true;
}

// ---------------------------------------------------------------------------
// Name prompt

bool name_dialog(QWidget* owner, const std::string& title, const std::string& prompt, std::string& name) {
    DialogPtr dlg(make_dialog(owner, title));
    auto* layout = new QVBoxLayout(dlg.get());
    auto* label = plain_label(qs(prompt), dlg.get());
    auto* edit = new QLineEdit(qs(name), dlg.get());
    edit->setMinimumWidth(320);
    label->setBuddy(edit);
    layout->addWidget(label);
    layout->addWidget(edit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg.get());
    layout->addWidget(buttons);
    std::string result;
    QObject::connect(buttons, &QDialogButtonBox::accepted, dlg.get(), [&] {
        const std::string n = trim(us(edit->text()));
        std::string why;
        if (!UserDirs::valid_profile_name(n, why)) {
            error_box(dlg.get(), why);
            return;
        }
        result = n;
        dlg->accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg.get(), &QDialog::reject);
    edit->selectAll();
    edit->setFocus();
    if (dlg->exec() != QDialog::Accepted) return false;
    name = result;
    return true;
}

// ---------------------------------------------------------------------------
// TLS settings

namespace {

// A certificate fetch runs on its own thread; the dialog may close first.
struct FetchJob {
    std::mutex mu;
    QPointer<QDialog> target;   // null once the dialog is gone or another fetch started
    bool ok = false;
    tn5250::PeerCertificate peer;
    std::string error;
};

// The session whose certificate Fetch reads: the first that uses TLS
// (session 1 when none does yet).
size_t tls_fetch_session(const Profile& p) {
    for (size_t i = 0; i < Profile::kSessions; ++i)
        if (p.sessions[i].used() && p.sessions[i].tls) return i;
    return 0;
}

} // namespace

bool tls_settings_dialog(QWidget* owner, Profile& p, const AppSettings& s) {
    DialogPtr dlg(make_dialog(owner, "TLS Settings"));
    auto* layout = new QVBoxLayout(dlg.get());
    auto* group = new QGroupBox(QStringLiteral("Server certificate"), dlg.get());
    auto* gl = new QGridLayout(group);
    auto* system = new QRadioButton(QStringLiteral("&Verify normally (the system's certificate authorities and the "
                                                   "host name)"),
                                    group);
    auto* any = new QRadioButton(QStringLiteral("&Accept all certificates (insecure)"), group);
    auto* hash = new QRadioButton(QStringLiteral("Accept only the certificate with this &SHA-256 hash:"), group);
    auto* hash_edit = new QLineEdit(qs(p.tls_cert_sha256), group);
    hash_edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    hash_edit->setMinimumWidth(hash_edit->fontMetrics().horizontalAdvance(QLatin1Char('0')) * 97);
    auto* fetch = new QPushButton(QStringLiteral("&Fetch"), group);
    const size_t fs = tls_fetch_session(p);
    const ProfileSession& src = p.sessions[fs];
    auto* fetch_host = plain_label(qs(src.used() ? format("from %s:%u (session %zu)", src.host.c_str(),
                                                        unsigned(src.port), fs + 1)
                                                 : std::string("enter the session 1 host first")),
                                  group);
    fetch->setEnabled(src.used());
    gl->addWidget(system, 0, 0, 1, 3);
    gl->addWidget(any, 1, 0, 1, 3);
    gl->addWidget(hash, 2, 0, 1, 3);
    gl->addWidget(hash_edit, 3, 0, 1, 3);
    gl->addWidget(fetch, 4, 0);
    gl->addWidget(fetch_host, 4, 1, 1, 2);
    auto* cert = details_box(group, "", 9);
    gl->addWidget(cert, 5, 0, 1, 3);
    layout->addWidget(group);
    (p.tls_mode == ProfileTlsMode::System      ? system
     : p.tls_mode == ProfileTlsMode::AcceptAll ? any
                                               : hash)
        ->setChecked(true);
    auto enable = [=] { hash_edit->setEnabled(hash->isChecked()); };
    enable();
    for (QRadioButton* r : {system, any, hash}) QObject::connect(r, &QRadioButton::toggled, dlg.get(), enable);

    auto* minform = new QFormLayout;
    auto* minver = new QComboBox(dlg.get());
    minver->addItem(qs("Default (" + tls_version_label(s.tls_min_version) + ")"));
    for (int i = 0; i < kTlsVersionCount; ++i) {
        minver->addItem(qs(tls_version_label(kTlsVersions[i])));
        TlsVersion v;
        if (!p.tls_min_version.empty() && parse_tls_version(p.tls_min_version, v) && v == kTlsVersions[i])
            minver->setCurrentIndex(i + 1);
    }
    minform->addRow(QStringLiteral("&Minimum version:"), minver);
    layout->addLayout(minform);

    std::shared_ptr<FetchJob> job;
    QDialog* self = dlg.get();
    QObject::connect(fetch, &QPushButton::clicked, self, [&, self] {
        if (job) {
            std::lock_guard<std::mutex> g(job->mu);
            job->target = nullptr;   // forget an earlier fetch still running
        }
        job = std::make_shared<FetchJob>();
        job->target = self;
        Tn5250Config cfg;
        cfg.host = src.host;
        cfg.port = src.port;
        cfg.tls = true;
        cfg.tls_min_version = s.tls_min_version;
        if (minver->currentIndex() > 0) cfg.tls_min_version = kTlsVersions[minver->currentIndex() - 1];
        fetch->setEnabled(false);
        cert->setPlainText(qs(format("Connecting to %s:%u...", cfg.host.c_str(), unsigned(cfg.port))));
        auto j = job;
        std::thread([j, cfg, cert, fetch, hash, hash_edit] {
            tn5250::PeerCertificate peer;
            std::string error;
            const bool ok = tn5250::tls_fetch_certificate(cfg, peer, error);
            std::lock_guard<std::mutex> g(j->mu);
            j->ok = ok;
            j->peer = peer;
            j->error = error;
            if (!j->target) return;
            // Back on the UI thread; the dialog may have closed by then, and
            // the call is dropped with it (its context object).
            QMetaObject::invokeMethod(
                j->target.data(),
                [j, cert, fetch, hash, hash_edit] {
                    std::lock_guard<std::mutex> inner(j->mu);
                    if (!j->target) return;
                    fetch->setEnabled(true);
                    if (j->ok) {
                        cert->setPlainText(qs(certificate_text(j->peer)));
                        if (j->peer.have_digest) {
                            hash_edit->setText(qs(format_sha256_fingerprint(j->peer.sha256)));
                            hash->setChecked(true);
                        }
                    } else {
                        cert->setPlainText(qs("Could not fetch the certificate:\n" + j->error));
                    }
                },
                Qt::QueuedConnection);
        }).detach();
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg.get());
    layout->addWidget(buttons);
    Profile result = p;
    QObject::connect(buttons, &QDialogButtonBox::accepted, self, [&] {
        Profile q = p;
        q.tls_mode = any->isChecked()    ? ProfileTlsMode::AcceptAll
                     : hash->isChecked() ? ProfileTlsMode::Fingerprint
                                         : ProfileTlsMode::System;
        q.tls_cert_sha256 = trim(us(hash_edit->text()));
        Sha256Digest digest;
        if (!q.tls_cert_sha256.empty() && parse_sha256_fingerprint(q.tls_cert_sha256, digest))
            q.tls_cert_sha256 = format_sha256_fingerprint(digest);   // canonical form
        if (q.tls_mode == ProfileTlsMode::Fingerprint && !parse_sha256_fingerprint(q.tls_cert_sha256, digest)) {
            error_box(self, "The certificate hash must be a SHA-256 fingerprint: 32 hex bytes, colons optional. "
                            "Use Fetch to read it from the server.");
            return;
        }
        const int mv = minver->currentIndex();
        q.tls_min_version = mv > 0 ? tls_version_name(kTlsVersions[mv - 1]) : "";
        result = q;
        self->accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, self, &QDialog::reject);
    const bool accepted = dlg->exec() == QDialog::Accepted;
    if (job) {
        std::lock_guard<std::mutex> g(job->mu);
        job->target = nullptr;
    }
    if (!accepted) return false;
    p = result;
    return true;
}

// ---------------------------------------------------------------------------
// Profile editor

namespace {

// One entry of the terminal type list: a variant of a model.
struct TerminalChoice {
    const Model* model;
    const ModelVariant* variant;
    bool ready;   // the model has its ROM images
};

std::string tls_summary(const Profile& p, const AppSettings& s) {
    std::string m;
    switch (p.tls_mode) {
    case ProfileTlsMode::System: m = "verify normally"; break;
    case ProfileTlsMode::AcceptAll: m = "accept all certificates (insecure)"; break;
    case ProfileTlsMode::Fingerprint: m = "accept the certificate " + p.tls_cert_sha256.substr(0, 23) + "..."; break;
    }
    TlsVersion v = s.tls_min_version;
    if (!p.tls_min_version.empty()) parse_tls_version(p.tls_min_version, v);
    return m + ", minimum " + tls_version_label(v);
}

} // namespace

bool profile_editor_dialog(QWidget* owner, Profile& p_in, ProfileEditMode mode, const AppSettings& s,
                           const UserDirs& dirs, const std::string& title) {
    Profile p = p_in;
    p.normalize();
    DialogPtr dlg(make_dialog(owner, title));
    QDialog* self = dlg.get();
    auto* layout = new QVBoxLayout(self);

    auto* terminal = new QGroupBox(QStringLiteral("Terminal"), self);
    auto* tf = new QFormLayout(terminal);
    auto* model = new QComboBox(terminal);
    auto* tube = new QComboBox(terminal);
    auto* crt = new QCheckBox(QStringLiteral("&Scan lines (CRT rendering)"), terminal);
    tf->addRow(QStringLiteral("Terminal &type:"), model);
    tf->addRow(QStringLiteral("&CRT type:"), tube);
    tf->addRow(QString(), crt);
    layout->addWidget(terminal);
    crt->setChecked(p.crt);

    std::vector<TerminalChoice> choices;
    int model_sel = 0;   // the selection to fall back to
    bool filling = false;
    // Every variant of every model that can be emulated; one whose model
    // lacks its ROM images is grey and says so.
    auto fill_models = [&](const std::string& select) {
        filling = true;
        model->clear();
        choices.clear();
        int sel = 0;
        const QColor grey = self->palette().color(QPalette::Disabled, QPalette::Text);
        for (const Model* m : runnable_models()) {
            for (const ModelVariant& v : m->info().variants) {
                const bool ready = model_roms_ready(*m, dirs.roms(), &v);
                model->addItem(qs("IBM " + v.id + " (" + v.description + (ready ? ")" : ", ROM images needed)")));
                if (!ready) model->setItemData(model->count() - 1, grey, Qt::ForegroundRole);
                if (v.id == select) sel = int(choices.size());
                choices.push_back(TerminalChoice{m, &v, ready});
            }
        }
        model->setCurrentIndex(sel);
        model_sel = sel;
        filling = false;
    };
    // The tubes the selected variant can be shown with; a colour unit has
    // one, and the list says so.
    auto fill_tubes = [&](const std::string& select) {
        tube->clear();
        const int sel = model->currentIndex();
        const ModelVariant* v = sel >= 0 && size_t(sel) < choices.size() ? choices[size_t(sel)].variant : nullptr;
        if (!v || v->phosphors.empty()) {
            tube->addItem(v ? QStringLiteral("Colour") : QString());
            tube->setEnabled(false);
            return;
        }
        int at = 0;
        for (size_t k = 0; k < v->phosphors.size(); ++k) {
            tube->addItem(qs(v->phosphors[k].label));
            if (v->phosphors[k].id == select) at = int(k);
        }
        tube->setCurrentIndex(at);
        tube->setEnabled(true);
    };
    auto selected_tube = [&]() -> std::string {
        const int sel = model->currentIndex();
        if (sel < 0 || size_t(sel) >= choices.size()) return {};
        const std::vector<Phosphor>& tubes = choices[size_t(sel)].variant->phosphors;
        const int st = tube->currentIndex();
        return st >= 0 && size_t(st) < tubes.size() ? tubes[size_t(st)].id : std::string();
    };
    fill_models(p.variant);
    fill_tubes(p.phosphor);
    QObject::connect(model, qOverload<int>(&QComboBox::currentIndexChanged), self, [&](int sel) {
        if (filling || sel < 0 || size_t(sel) >= choices.size()) return;
        guarded(self, [&] {
            // The tube carries over when the new variant has it too.
            const std::string keep = p.phosphor;
            const TerminalChoice choice = choices[size_t(sel)];
            if (choice.ready) {
                model_sel = sel;
                fill_tubes(keep);
                p.phosphor = selected_tube();
                return;
            }
            // A model without its ROM images: say which files, offer to
            // import them, and go back when they are still not there.
            const std::string back = choices[size_t(model_sel)].variant->id;
            const bool ok = ensure_model_roms(self, dirs, *choice.model, "", choice.variant);
            fill_models(ok ? choice.variant->id : back);   // an import can ready several models
            fill_tubes(keep);
            p.phosphor = selected_tube();
        });
    });
    QObject::connect(tube, qOverload<int>(&QComboBox::currentIndexChanged), self,
                     [&](int) { p.phosphor = selected_tube(); });

    auto* sessions = new QGroupBox(QStringLiteral("Sessions"), self);
    auto* sg = new QGridLayout(sessions);
    sg->addWidget(plain_label(QStringLiteral("Host"), sessions), 0, 1);
    sg->addWidget(plain_label(QStringLiteral("Port"), sessions), 0, 2);
    sg->addWidget(plain_label(QStringLiteral("Device name (optional)"), sessions), 0, 3);
    QLineEdit* host[2];
    QLineEdit* port[2];
    QLineEdit* dev[2];
    QCheckBox* use_tls[2];
    for (int i = 0; i < 2; ++i) {
        const ProfileSession& ps = p.sessions[size_t(i)];
        sg->addWidget(plain_label(qs(format("Session &%d:", i + 1)), sessions), i + 1, 0);
        host[i] = new QLineEdit(qs(ps.host), sessions);
        host[i]->setMinimumWidth(220);
        port[i] = new QLineEdit(qs(ps.used() || i == 0 ? std::to_string(ps.port) : std::string()), sessions);
        port[i]->setMaxLength(5);
        port[i]->setMaximumWidth(port[i]->fontMetrics().horizontalAdvance(QStringLiteral("000000")) + 16);
        dev[i] = new QLineEdit(qs(ps.device_name), sessions);
        dev[i]->setMaxLength(10);
        use_tls[i] = new QCheckBox(QStringLiteral("Use TLS"), sessions);
        use_tls[i]->setChecked(ps.tls);
        static_cast<QLabel*>(sg->itemAtPosition(i + 1, 0)->widget())->setBuddy(host[i]);
        sg->addWidget(host[i], i + 1, 1);
        sg->addWidget(port[i], i + 1, 2);
        sg->addWidget(dev[i], i + 1, 3);
        sg->addWidget(use_tls[i], i + 1, 4);
    }
    auto* note = plain_label(QStringLiteral("Session 2 is optional: a second display station, on the same system or "
                                           "another."),
                            sessions);
    note->setWordWrap(true);
    sg->addWidget(note, 3, 0, 1, 5);
    layout->addWidget(sessions);

    auto* tls_row = new QHBoxLayout;
    auto* tls_summary_label = plain_label(self);
    tls_summary_label->setWordWrap(true);
    auto* tls_button = new QPushButton(QStringLiteral("TLS &Settings..."), self);
    tls_row->addWidget(tls_summary_label, 1);
    tls_row->addWidget(tls_button);
    layout->addLayout(tls_row);

    // The fields into p; false (with a message) when a port is invalid.
    auto read = [&](bool quiet) {
        const int m = model->currentIndex();
        if (m >= 0 && size_t(m) < choices.size()) {
            p.model = choices[size_t(m)].model->info().name;
            p.variant = choices[size_t(m)].variant->id;
            p.phosphor = selected_tube();
        }
        p.crt = crt->isChecked();
        for (size_t i = 0; i < 2; ++i) {
            ProfileSession& ss = p.sessions[i];
            ss.host = trim(us(host[i]->text()));
            ss.device_name = trim(us(dev[i]->text()));
            ss.tls = use_tls[i]->isChecked();
            const std::string pt = trim(us(port[i]->text()));
            uint64_t v = 0;
            if (pt.empty()) v = ss.tls ? 992 : 23;
            else if (!parse_uint(pt, v) || v == 0 || v > 65535) {
                if (!quiet) error_box(self, format("Session %zu: the port must be a number from 1 to 65535.", i + 1));
                return false;
            }
            ss.port = uint16_t(v);
        }
        return true;
    };
    auto update_tls = [&] {
        std::string which;
        for (size_t i = 0; i < 2; ++i)
            if (use_tls[i]->isChecked())
                which += which.empty() ? format("session %zu", i + 1) : format(" and %zu", i + 1);
        tls_button->setEnabled(!which.empty());
        tls_summary_label->setText(qs(which.empty() ? std::string("No session uses TLS: plain telnet.")
                                                    : "TLS for " + which + ": " + tls_summary(p, s) + "."));
    };
    update_tls();
    for (int i = 0; i < 2; ++i) {
        QObject::connect(use_tls[i], &QCheckBox::toggled, self, [&, i](bool on) {
            // The session's conventional port changes along with TLS.
            const std::string pt = trim(us(port[i]->text()));
            if (pt == (on ? "23" : "992")) port[i]->setText(on ? QStringLiteral("992") : QStringLiteral("23"));
            update_tls();
        });
    }
    QObject::connect(tls_button, &QPushButton::clicked, self, [&] {
        guarded(self, [&] {
            read(true);
            if (tls_settings_dialog(self, p, s)) update_tls();
        });
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, self);
    buttons->button(QDialogButtonBox::Ok)
        ->setText(mode == ProfileEditMode::Connect ? QStringLiteral("Connect")
                  : mode == ProfileEditMode::Save  ? QStringLiteral("Save")
                                                   : QStringLiteral("OK"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, self, [&] {
        if (!read(false)) return;
        const std::string why = p.validate();
        if (!why.empty()) {
            error_box(self, why);
            return;
        }
        self->accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, self, &QDialog::reject);
    host[0]->setFocus();
    if (dlg->exec() != QDialog::Accepted) return false;
    p_in = p;
    return true;
}

// ---------------------------------------------------------------------------
// Profile manager

std::string profile_manager_dialog(QWidget* owner, const UserDirs& dirs, const AppSettings& s) {
    DialogPtr dlg(make_dialog(owner, "Connect to Profile"));
    QDialog* self = dlg.get();
    auto* layout = new QHBoxLayout(self);
    auto* list = new QListWidget(self);
    list->setMinimumSize(280, 260);
    layout->addWidget(list, 1);
    auto* column = new QVBoxLayout;
    auto* connect = new QPushButton(QStringLiteral("&Connect"), self);
    connect->setDefault(true);
    auto* add = new QPushButton(QStringLiteral("&New..."), self);
    auto* edit = new QPushButton(QStringLiteral("&Edit..."), self);
    auto* rename = new QPushButton(QStringLiteral("&Rename..."), self);
    auto* copy = new QPushButton(QStringLiteral("Co&py"), self);
    auto* del = new QPushButton(QStringLiteral("&Delete"), self);
    auto* close = new QPushButton(QStringLiteral("Close"), self);
    for (QPushButton* b : {connect, add, edit, rename, copy, del}) column->addWidget(b);
    column->addStretch(1);
    column->addWidget(close);
    layout->addLayout(column);

    std::vector<std::string> names;
    std::string chosen;
    auto fill = [&](const std::string& select) {
        list->clear();
        names = dirs.list_profiles();
        int sel = names.empty() ? -1 : 0;
        for (size_t i = 0; i < names.size(); ++i) {
            list->addItem(qs(names[i]));
            if (names[i] == select) sel = int(i);
        }
        list->setCurrentRow(sel);
        for (QPushButton* b : {connect, edit, rename, copy, del}) b->setEnabled(sel >= 0);
    };
    auto selected = [&]() -> std::string {
        const int i = list->currentRow();
        return i >= 0 && size_t(i) < names.size() ? names[size_t(i)] : std::string();
    };
    fill(std::string());

    auto choose = [&] {
        const std::string sel = selected();
        if (sel.empty()) return;
        chosen = sel;
        self->accept();
    };
    QObject::connect(list, &QListWidget::itemActivated, self, [&](QListWidgetItem*) { choose(); });
    QObject::connect(connect, &QPushButton::clicked, self, choose);
    QObject::connect(add, &QPushButton::clicked, self, [&] {
        guarded(self, [&] {
            std::string name = dirs.unique_profile_name("New Profile");
            if (!name_dialog(self, "New Profile", "&Name of the new profile:", name)) return;
            if (dirs.profile_exists(name)) {
                error_box(self, "A profile named \"" + name + "\" already exists.");
                return;
            }
            Profile p;
            // A new profile is for a terminal that can run.
            const StartModel start = choose_start_model(p.model, dirs.roms());
            if (start.model) {
                p.model = start.model->info().name;
                if (start.variant) p.variant = start.variant->id;
            }
            if (!profile_editor_dialog(self, p, ProfileEditMode::Save, s, dirs, "New Profile: " + name)) return;
            std::string error;
            if (!save_profile(dirs.profile_path(name), p, error)) error_box(self, error);
            fill(name);
        });
    });
    QObject::connect(edit, &QPushButton::clicked, self, [&] {
        guarded(self, [&] {
            const std::string sel = selected();
            if (sel.empty()) return;
            Profile p;
            std::string error;
            if (!load_profile(dirs.profile_path(sel), p, error)) {
                error_box(self, error);
                return;
            }
            if (!profile_editor_dialog(self, p, ProfileEditMode::Save, s, dirs, "Profile: " + sel)) return;
            if (!save_profile(dirs.profile_path(sel), p, error)) error_box(self, error);
        });
    });
    QObject::connect(rename, &QPushButton::clicked, self, [&] {
        guarded(self, [&] {
            const std::string sel = selected();
            if (sel.empty()) return;
            std::string name = sel;
            if (!name_dialog(self, "Rename Profile", "&New name:", name) || name == sel) return;
            std::string error;
            if (!dirs.rename_profile(sel, name, error)) error_box(self, error);
            fill(name);
        });
    });
    QObject::connect(copy, &QPushButton::clicked, self, [&] {
        guarded(self, [&] {
            const std::string sel = selected();
            if (sel.empty()) return;
            const std::string name = dirs.unique_profile_name(sel + " (copy)");
            std::string error;
            if (!dirs.copy_profile(sel, name, error)) error_box(self, error);
            fill(name);
        });
    });
    QObject::connect(del, &QPushButton::clicked, self, [&] {
        guarded(self, [&] {
            const std::string sel = selected();
            if (sel.empty()) return;
            if (!ask_yes_no(self, "Delete the profile \"" + sel + "\"?")) return;
            std::string error;
            if (!dirs.delete_profile(sel, error)) error_box(self, error);
            fill(std::string());
        });
    });
    QObject::connect(close, &QPushButton::clicked, self, &QDialog::reject);
    list->setFocus();
    if (dlg->exec() != QDialog::Accepted) return std::string();
    return chosen;
}

// ---------------------------------------------------------------------------
// Trust prompts

namespace {

QLabel* warning_icon(QWidget* parent) {
    auto* icon = new QLabel(parent);
    const int size = parent->style()->pixelMetric(QStyle::PM_MessageBoxIconSize, nullptr, parent);
    icon->setPixmap(parent->style()->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, parent).pixmap(size, size));
    icon->setAlignment(Qt::AlignTop);
    return icon;
}

} // namespace

tn5250::TlsTrustDecision trust_dialog(QWidget* owner, const tn5250::TlsTrustRequest& r, const std::string& profile) {
    DialogPtr dlg(make_dialog(owner, "Untrusted Certificate"));
    QDialog* self = dlg.get();
    auto* layout = new QVBoxLayout(self);
    auto* top = new QHBoxLayout;
    top->addWidget(warning_icon(self));
    std::string text = format("The server certificate of %s:%u (session %u) is not trusted", r.host.c_str(),
                              unsigned(r.port), r.station + 1);
    text += r.problem == tn5250::TlsTrustProblem::PinMismatch
                ? ": it is not the certificate this profile trusts. The server may have a new certificate, or "
                  "someone may be intercepting the connection."
                : ". Someone may be intercepting the connection; connect only if you know this certificate.";
    auto* headline = plain_label(qs(text), self);
    headline->setWordWrap(true);
    top->addWidget(headline, 1);
    layout->addLayout(top);
    std::string details = "Reason:\t" + r.reason + "\n\n" + certificate_text(r.peer);
    details += "\nTrust Permanently stores the SHA-256 in the profile \"" + profile +
               "\"; only this certificate is accepted afterwards.";
    layout->addWidget(details_box(self, details, 12));
    auto* buttons = new QDialogButtonBox(self);
    QPushButton* stop = add_button(buttons, "&Stop", QDialogButtonBox::RejectRole);
    QPushButton* once = add_button(buttons, "&Connect", QDialogButtonBox::AcceptRole);
    QPushButton* always = add_button(buttons, "&Trust Permanently", QDialogButtonBox::AcceptRole);
    stop->setDefault(true);
    layout->addWidget(buttons);
    tn5250::TlsTrustDecision decision = tn5250::TlsTrustDecision::Reject;
    QObject::connect(stop, &QPushButton::clicked, self, &QDialog::reject);
    QObject::connect(once, &QPushButton::clicked, self, [&] {
        decision = tn5250::TlsTrustDecision::AcceptOnce;
        self->accept();
    });
    QObject::connect(always, &QPushButton::clicked, self, [&] {
        decision = tn5250::TlsTrustDecision::AcceptPermanently;
        self->accept();
    });
    stop->setFocus();
    QApplication::beep();
    if (dlg->exec() != QDialog::Accepted) return tn5250::TlsTrustDecision::Reject;
    return decision;
}

bool certificate_invalid_dialog(QWidget* owner, const tn5250::TlsTrustRequest& r) {
    DialogPtr dlg(make_dialog(owner, "Certificate Not Valid"));
    QDialog* self = dlg.get();
    auto* layout = new QVBoxLayout(self);
    auto* top = new QHBoxLayout;
    top->addWidget(warning_icon(self));
    auto* headline = plain_label(
        qs(format("The server certificate of %s:%u (session %u) is not valid. The connection may not be secure: the "
                  "server may be badly maintained, this computer's clock may be wrong, or someone may be "
                  "intercepting the connection.",
                  r.host.c_str(), unsigned(r.port), r.station + 1)),
        self);
    headline->setWordWrap(true);
    top->addWidget(headline, 1);
    layout->addLayout(top);
    layout->addWidget(plain_label(QStringLiteral("What is wrong with the certificate:"), self));
    std::string errors;
    for (const std::string& e : r.errors) errors += e + "\n";
    if (errors.empty()) errors = r.reason + "\n";
    layout->addWidget(details_box(self, errors, 3));
    layout->addWidget(plain_label(QStringLiteral("The certificate the server sent:"), self));
    layout->addWidget(details_box(self, certificate_text(r.peer), 8));
    layout->addWidget(
        plain_label(QStringLiteral("This cannot be remembered: you are asked again for every connection."), self));
    auto* understand =
        new QCheckBox(QStringLiteral("&I understand the risk and want to connect to this server anyway"), self);
    layout->addWidget(understand);
    auto* buttons = new QDialogButtonBox(self);
    QPushButton* cancel = buttons->addButton(QDialogButtonBox::Cancel);
    QPushButton* go = add_button(buttons, "Connect &Anyway", QDialogButtonBox::AcceptRole);
    go->setEnabled(false);
    go->setAutoDefault(false);
    cancel->setDefault(true);
    layout->addWidget(buttons);
    QObject::connect(understand, &QCheckBox::toggled, go, &QWidget::setEnabled);
    QObject::connect(cancel, &QPushButton::clicked, self, &QDialog::reject);
    QObject::connect(go, &QPushButton::clicked, self, [&] {
        if (understand->isChecked()) self->accept();
    });
    cancel->setFocus();
    QApplication::beep();
    return dlg->exec() == QDialog::Accepted && understand->isChecked();
}

// ---------------------------------------------------------------------------
// Keymap editor

namespace {

void mods_flags(const std::string& mods, bool& shift, bool& ctrl, bool& alt, bool& any) {
    shift = ctrl = alt = any = false;
    for (const std::string& w : split_words(to_lower(mods))) {
        if (w == "*" || w == "any") any = true;
        else if (w.find("shift") != std::string::npos) shift = true;
        else if (w.find("ctrl") != std::string::npos || w == "control") ctrl = true;
        else if (w.find("alt") != std::string::npos) alt = true;
    }
}

const char* const kKeyboards[] = {"Both", "122-key", "Enhanced"};
const char* const kKeyboardValues[] = {"", "122", "enhanced"};

// Click it, then press a key: the key by position (as the terminal gets
// it) and its modifiers. Every key is taken while it has the focus, Tab
// included; click elsewhere to leave it.
class KeyCapture : public QFrame {
public:
    std::function<void(uint32_t hid, bool shift, bool ctrl, bool alt)> on_key;

    explicit KeyCapture(QWidget* parent) : QFrame(parent) {
        setObjectName(QStringLiteral("capture"));
        setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(fontMetrics().height() * 2);
        label_ = plain_label(QStringLiteral("(click here, then press a key)"), this);
        label_->setAlignment(Qt::AlignCenter);
        auto* l = new QHBoxLayout(this);
        l->addWidget(label_);
    }
    void show_text(const QString& t) { label_->setText(t); }

protected:
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ShortcutOverride) {
            e->accept();
            return true;
        }
        if (e->type() == QEvent::KeyPress) {
            auto* k = static_cast<QKeyEvent*>(e);
            const uint32_t hid = hid_from_xkb_keycode(k->nativeScanCode());
            if (hid && on_key) {
                const Qt::KeyboardModifiers m = k->modifiers();
                on_key(hid, m & Qt::ShiftModifier, m & Qt::ControlModifier, m & Qt::AltModifier);
            }
            return true;
        }
        if (e->type() == QEvent::KeyRelease) return true;
        return QFrame::event(e);
    }
    void focusInEvent(QFocusEvent* e) override {
        setStyleSheet(QStringLiteral("QFrame#capture { border: 2px solid palette(highlight); }"));
        QFrame::focusInEvent(e);
    }
    void focusOutEvent(QFocusEvent* e) override {
        setStyleSheet(QString());
        QFrame::focusOutEvent(e);
    }

private:
    QLabel* label_;
};

bool entry_dialog(QWidget* owner, KeymapEntry& entry) {
    DialogPtr dlg(make_dialog(owner, "Key Mapping"));
    QDialog* self = dlg.get();
    auto* layout = new QVBoxLayout(self);
    auto* form = new QFormLayout;
    auto* capture = new KeyCapture(self);
    form->addRow(QStringLiteral("Press the PC key:"), capture);
    auto* host = new QComboBox(self);
    host->setEditable(true);
    for (uint32_t hid = 0x04; hid <= 0xE7; ++hid) {
        const std::string n = host_key_name(hid);
        if (n.rfind("0x", 0) != 0) host->addItem(qs(n));
    }
    host->setEditText(qs(entry.host));
    form->addRow(QStringLiteral("or choose the &PC key:"), host);
    auto* mods_row = new QHBoxLayout;
    auto* shift = new QCheckBox(QStringLiteral("&Shift"), self);
    auto* ctrl = new QCheckBox(QStringLiteral("C&trl"), self);
    auto* alt = new QCheckBox(QStringLiteral("A&lt"), self);
    auto* any = new QCheckBox(QStringLiteral("Any &modifiers"), self);
    for (QCheckBox* c : {shift, ctrl, alt, any}) mods_row->addWidget(c);
    mods_row->addStretch(1);
    form->addRow(QStringLiteral("Modifiers:"), mods_row);
    auto* to = new QComboBox(self);
    to->setEditable(true);
    to->addItem(QStringLiteral("none"));
    to->addItem(QStringLiteral("positional"));
    for (const std::string& f : terminal_function_names()) to->addItem(qs(f));
    to->setEditText(qs(entry.to));
    form->addRow(QStringLiteral("Terminal &key:"), to);
    auto* keyboard = new QComboBox(self);
    for (int i = 0; i < 3; ++i) {
        keyboard->addItem(qs(kKeyboards[i]));
        if (to_lower(entry.keyboard) == kKeyboardValues[i] || (i == 0 && to_lower(entry.keyboard) == "both"))
            keyboard->setCurrentIndex(i);
    }
    form->addRow(QStringLiteral("Key&board:"), keyboard);
    auto* tap = new QCheckBox(QStringLiteral("T&ap (press and release at once)"), self);
    tap->setChecked(entry.tap);
    form->addRow(QString(), tap);
    layout->addLayout(form);

    bool o_shift, o_ctrl, o_alt, o_any;
    mods_flags(entry.mods, o_shift, o_ctrl, o_alt, o_any);
    shift->setChecked(o_shift);
    ctrl->setChecked(o_ctrl);
    alt->setChecked(o_alt);
    any->setChecked(o_any);

    capture->on_key = [&](uint32_t hid, bool sh, bool ct, bool al) {
        const uint8_t own = host_modifier_bit(hid);
        // A modifier pressed on its own is the key itself, without modifiers.
        const bool m_shift = sh && !(own & (kModLShift | kModRShift));
        const bool m_ctrl = ct && !(own & (kModLCtrl | kModRCtrl));
        const bool m_alt = al && !(own & (kModLAlt | kModRAlt));
        const std::string name = host_key_name(hid);
        host->setEditText(qs(name));
        shift->setChecked(m_shift);
        ctrl->setChecked(m_ctrl);
        alt->setChecked(m_alt);
        capture->show_text(qs((m_ctrl ? "Ctrl+" : "") + std::string(m_alt ? "Alt+" : "") +
                              (m_shift ? "Shift+" : "") + name));
    };

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, self);
    layout->addWidget(buttons);
    KeymapEntry result = entry;
    QObject::connect(buttons, &QDialogButtonBox::accepted, self, [&] {
        KeymapEntry e = entry;
        e.host = trim(us(host->currentText()));
        e.to = trim(us(to->currentText()));
        if (shift->isChecked() == o_shift && ctrl->isChecked() == o_ctrl && alt->isChecked() == o_alt &&
            any->isChecked() == o_any) {
            e.mods = entry.mods;   // keep left/right detail the boxes cannot show
        } else {
            std::string m;
            if (shift->isChecked()) m += "shift ";
            if (ctrl->isChecked()) m += "ctrl ";
            if (alt->isChecked()) m += "alt ";
            if (any->isChecked()) m += "*";
            e.mods = trim(m);
        }
        const int kb = keyboard->currentIndex();
        e.keyboard = kb > 0 && kb < 3 ? kKeyboardValues[kb] : "";
        e.tap = tap->isChecked();
        std::string error;
        if (!check_keymap_entry(e, error)) {
            error_box(self, error);
            return;
        }
        result = e;
        self->accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, self, &QDialog::reject);
    if (dlg->exec() != QDialog::Accepted) return false;
    entry = result;
    return true;
}

} // namespace

bool keymap_dialog(QWidget* owner, KeymapFile& file_in) {
    KeymapFile file = file_in;
    DialogPtr dlg(make_dialog(owner, "Keymap"));
    QDialog* self = dlg.get();
    auto* layout = new QVBoxLayout(self);
    auto* row = new QHBoxLayout;
    auto* list = new QTreeWidget(self);
    list->setColumnCount(5);
    list->setHeaderLabels({QStringLiteral("PC key"), QStringLiteral("Modifiers"), QStringLiteral("Terminal key"),
                           QStringLiteral("Keyboard"), QStringLiteral("Options")});
    list->setRootIsDecorated(false);
    list->setAlternatingRowColors(true);
    list->setMinimumSize(560, 320);
    row->addWidget(list, 1);
    auto* column = new QVBoxLayout;
    auto* add = new QPushButton(QStringLiteral("&Add..."), self);
    auto* edit = new QPushButton(QStringLiteral("&Edit..."), self);
    auto* remove = new QPushButton(QStringLiteral("&Remove"), self);
    auto* reset = new QPushButton(QStringLiteral("Reset to &Default (PCOMM)"), self);
    auto* tn5250 = new QPushButton(QStringLiteral("Use &tn5250 Layout"), self);
    for (QPushButton* b : {add, edit, remove}) column->addWidget(b);
    column->addSpacing(12);
    column->addWidget(reset);
    column->addWidget(tn5250);
    column->addStretch(1);
    row->addLayout(column);
    layout->addLayout(row);
    auto* base = plain_label(self);
    base->setWordWrap(true);
    layout->addWidget(base);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, self);
    layout->addWidget(buttons);

    auto fill = [&](int select) {
        list->clear();
        for (const KeymapEntry& e : file.entries) {
            std::string opt;
            if (e.tap) opt += "tap ";
            if (!e.shift.empty()) opt += "shift " + e.shift + " ";
            if (!e.alt.empty()) opt += "alt " + e.alt;
            auto* item = new QTreeWidgetItem(list);
            item->setText(0, qs(e.host));
            item->setText(1, qs(e.mods.empty() ? std::string("-") : e.mods == "*" ? std::string("any") : e.mods));
            item->setText(2, qs(e.to));
            item->setText(3, qs(e.keyboard.empty() ? std::string("both") : e.keyboard));
            item->setText(4, qs(trim(opt)));
        }
        for (int c = 0; c < 5; ++c) list->resizeColumnToContents(c);
        const int n = list->topLevelItemCount();
        if (n > 0) list->setCurrentItem(list->topLevelItem(std::clamp(select, 0, n - 1)));
        base->setText(qs(keymap_base_text(file.base)));
    };
    fill(0);
    auto current = [&] { return list->indexOfTopLevelItem(list->currentItem()); };
    auto edit_current = [&] {
        guarded(self, [&] {
            const int sel = current();
            if (sel < 0 || size_t(sel) >= file.entries.size()) return;
            if (entry_dialog(self, file.entries[size_t(sel)])) fill(sel);
        });
    };
    QObject::connect(list, &QTreeWidget::itemActivated, self, [&](QTreeWidgetItem*, int) { edit_current(); });
    QObject::connect(edit, &QPushButton::clicked, self, edit_current);
    QObject::connect(add, &QPushButton::clicked, self, [&] {
        guarded(self, [&] {
            KeymapEntry e;
            e.host = "F1";
            e.to = "f1";
            if (!entry_dialog(self, e)) return;
            file.entries.push_back(e);
            fill(int(file.entries.size()) - 1);
        });
    });
    QObject::connect(remove, &QPushButton::clicked, self, [&] {
        const int sel = current();
        if (sel < 0 || size_t(sel) >= file.entries.size()) return;
        file.entries.erase(file.entries.begin() + sel);
        fill(sel);
    });
    QObject::connect(reset, &QPushButton::clicked, self, [&] {
        if (!ask_yes_no(self, "Replace the keymap with the default IBM Personal Communications (PCOMM) layout?"))
            return;
        file = pcomm_keymap_file();
        fill(0);
    });
    QObject::connect(tn5250, &QPushButton::clicked, self, [&] {
        if (!ask_yes_no(self, "Replace the keymap with the tn5250 layout? Return is Enter, and the 5250 functions "
                              "are Ctrl and Left Alt chords: for keyboards without a Right Ctrl or a keypad."))
            return;
        file = tn5250_keymap_file();
        fill(0);
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, self, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, self, &QDialog::reject);
    if (dlg->exec() != QDialog::Accepted) return false;
    file_in = file;
    return true;
}

// ---------------------------------------------------------------------------
// About

void about_dialog(QWidget* owner, const UserDirs& dirs) {
    DialogPtr dlg(make_dialog(owner, "About em34xx"));
    QDialog* self = dlg.get();
    auto* layout = new QVBoxLayout(self);
    auto* top = new QHBoxLayout;
    auto* icon = new QLabel(self);
    icon->setPixmap(QApplication::windowIcon().pixmap(64, 64));
    icon->setAlignment(Qt::AlignTop);
    top->addWidget(icon);
    const char* tls = tn5250::tls_backend_name();
    const std::string url = EM34XX_HOMEPAGE_URL;
    QString html =
        QStringLiteral("<b>em34xx-qt version %1</b><br>Hardware emulator for IBM 34xx twinax terminals, connected to "
                       "IBM i over TN5250.<br>Free software under the GNU GPL, version 3 or later; no warranty.<br><br>"
                       "User folder: %2<br>TLS: %3<br>Qt %4 (%5)<br><br>")
            .arg(QStringLiteral(EM34XX_VERSION_STRING), qs(dirs.root()).toHtmlEscaped(),
                 qs(tls ? tls : "none").toHtmlEscaped(), qs(qVersion()), QGuiApplication::platformName());
    html += url.empty() ? QStringLiteral("Project page: to be announced")
                        : QStringLiteral("Project page: <a href=\"%1\">%1</a>").arg(qs(url).toHtmlEscaped());
    auto* text = new QLabel(html, self);
    text->setTextFormat(Qt::RichText);
    text->setOpenExternalLinks(true);
    text->setTextInteractionFlags(Qt::TextBrowserInteraction);
    text->setWordWrap(true);
    top->addWidget(text, 1);
    layout->addLayout(top);
    layout->addWidget(plain_label(QStringLiteral("Third-party notices:"), self));
    QFile notices(QStringLiteral(":/notices.txt"));
    std::string body = "(notices not found)";
    if (notices.open(QIODevice::ReadOnly)) body = us(QString::fromUtf8(notices.readAll()));
    auto* box = details_box(self, body, 12);
    box->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    box->setLineWrapMode(QPlainTextEdit::NoWrap);
    box->setMinimumWidth(box->fontMetrics().horizontalAdvance(QLatin1Char('0')) * 84);
    layout->addWidget(box);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, self);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, self, &QDialog::accept);
    dlg->exec();
}

// ---------------------------------------------------------------------------
// File association

namespace {

bool write_file(const QString& path, const QByteArray& data, std::string& error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
        error = "cannot write " + us(path) + ": " + us(f.errorString());
        return false;
    }
    return true;
}

// Run a desktop database tool when it is installed; a missing one is not an
// error (the desktop then notices the files itself, or at the next login).
void run_tool(const QString& program, const QStringList& args) {
    const QString path = QStandardPaths::findExecutable(program);
    if (!path.isEmpty()) QProcess::execute(path, args);
}

// A desktop entry's Exec key: the argument quoted as the specification asks.
QString exec_quote(const QString& arg) {
    QString q = arg;
    for (const QChar c : {QLatin1Char('\\'), QLatin1Char('"'), QLatin1Char('`'), QLatin1Char('$')})
        q.replace(c, QString(QLatin1Char('\\')) + c);
    q.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return QLatin1Char('"') + q + QLatin1Char('"');
}

} // namespace

bool register_file_association(std::string& error) {
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (data.isEmpty()) {
        error = "cannot find the user's data folder ($XDG_DATA_HOME)";
        return false;
    }
    QFile mime(QStringLiteral(":/em34xx-mime.xml"));
    if (!mime.open(QIODevice::ReadOnly)) {
        error = "the file type description is missing from this build";
        return false;
    }
    if (!write_file(data + QStringLiteral("/mime/packages/em34xx.xml"), mime.readAll(), error)) return false;
    // The installed desktop entry, pointed at this executable.
    QFile entry(QStringLiteral(":/em34xx.desktop"));
    if (!entry.open(QIODevice::ReadOnly)) {
        error = "the desktop entry is missing from this build";
        return false;
    }
    QString text = QString::fromUtf8(entry.readAll());
    text.replace(QStringLiteral("Exec=em34xx-qt"),
                 QStringLiteral("Exec=") + exec_quote(QCoreApplication::applicationFilePath()));
    if (!write_file(data + QStringLiteral("/applications/em34xx.desktop"), text.toUtf8(), error)) return false;
    run_tool(QStringLiteral("update-mime-database"), {data + QStringLiteral("/mime")});
    run_tool(QStringLiteral("update-desktop-database"), {data + QStringLiteral("/applications")});
    // xdg-mime writes mimeapps.list there and fails when the folder is
    // missing.
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
    run_tool(QStringLiteral("xdg-mime"),
             {QStringLiteral("default"), QStringLiteral("em34xx.desktop"), QStringLiteral("application/x-em34xx-profile")});
    return true;
}

} // namespace em34xx::qt
