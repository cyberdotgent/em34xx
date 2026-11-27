// SPDX-License-Identifier: GPL-3.0-or-later
// Helpers for the Qt front end: UTF-8 <-> QString, message boxes, the user
// folder, the toolbar and status glyphs, the file manager. Qt 6 Widgets, no
// moc. See docs/frontend-qt.md.
#pragma once

#include <QIcon>
#include <QMessageBox>
#include <QString>

#include <exception>
#include <string>

class QLabel;
class QWidget;

namespace em34xx::qt {

inline QString qs(const std::string& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
inline QString qs(const char* s) { return QString::fromUtf8(s); }
inline std::string us(const QString& s) {
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), size_t(b.size()));
}

// A label that shows its text as it is (Qt::PlainText): text that comes
// from files (profile names, host names, paths) never renders as markup.
QLabel* plain_label(const QString& text, QWidget* parent);
QLabel* plain_label(QWidget* parent);
// A tooltip that shows `text` as it is, line breaks kept (QToolTip guesses
// rich text by itself; this is escaped rich text).
QString plain_tip(const QString& text);

// The title of every message box.
inline QString app_title() { return QStringLiteral("em34xx"); }

void error_box(QWidget* owner, const std::string& text);
void info_box(QWidget* owner, const std::string& text);
// Yes / No. `warning`: a warning icon (else a question); `default_no`: No is
// the default button.
bool ask_yes_no(QWidget* owner, const std::string& text, bool warning = false, bool default_no = false);

// Run `fn`; an exception it throws is reported in a message box and the
// operation abandoned (Qt's event handlers must not throw). The window
// equivalent of em34xxw's win32::guarded.
template <class F>
void guarded(QWidget* owner, F&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        error_box(owner, std::string("Unexpected error: ") + e.what());
    } catch (...) {
        error_box(owner, "Unexpected error.");
    }
}

// The user folder: EM34XX_USER_DIR when set (portable use, tests), else
// $XDG_DATA_HOME/em34xx (~/.local/share/em34xx). "" when neither can be
// found.
std::string user_folder();

// Whether `file` lies directly in `folder` (the same directory on disk,
// whatever the spelling of the paths).
bool file_in_folder(const std::string& file, const std::string& folder);

// The file manager on a folder, or the default application on a file.
void open_with_desktop(QWidget* owner, const std::string& path);

// A toolbar or status glyph ("save", "orb-green"...) in every size drawn
// (tools/gen_linux_assets.py, embedded under :/glyphs).
QIcon glyph(const char* name);

} // namespace em34xx::qt
