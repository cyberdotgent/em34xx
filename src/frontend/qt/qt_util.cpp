// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/qt/qt_util.hpp"

#include <QDesktopServices>
#include <QLabel>
#include <QStandardPaths>
#include <QUrl>
#include <QWidget>

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace em34xx::qt {

namespace {

int show_box(QWidget* owner, QMessageBox::Icon icon, const std::string& text, QMessageBox::StandardButtons buttons,
             QMessageBox::StandardButton def) {
    QMessageBox box(icon, app_title(), qs(text), buttons, owner);
    box.setDefaultButton(def);
    box.setTextFormat(Qt::PlainText);
    return box.exec();
}

} // namespace

QLabel* plain_label(const QString& text, QWidget* parent) {
    auto* l = new QLabel(parent);
    l->setTextFormat(Qt::PlainText);
    l->setText(text);
    return l;
}

QLabel* plain_label(QWidget* parent) { return plain_label(QString(), parent); }

QString plain_tip(const QString& text) {
    return QStringLiteral("<p style='white-space:pre'>") + text.toHtmlEscaped() + QStringLiteral("</p>");
}

void error_box(QWidget* owner, const std::string& text) {
    show_box(owner, QMessageBox::Critical, text, QMessageBox::Ok, QMessageBox::Ok);
}

void info_box(QWidget* owner, const std::string& text) {
    show_box(owner, QMessageBox::Information, text, QMessageBox::Ok, QMessageBox::Ok);
}

bool ask_yes_no(QWidget* owner, const std::string& text, bool warning, bool default_no) {
    return show_box(owner, warning ? QMessageBox::Warning : QMessageBox::Question, text,
                    QMessageBox::Yes | QMessageBox::No, default_no ? QMessageBox::No : QMessageBox::Yes) ==
           QMessageBox::Yes;
}

std::string user_folder() {
    if (const char* env = std::getenv("EM34XX_USER_DIR"); env && *env) return env;
    // $XDG_DATA_HOME, else ~/.local/share (QStandardPaths follows the XDG
    // base directory specification).
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (data.isEmpty()) return std::string();
    return us(data) + "/em34xx";
}

bool file_in_folder(const std::string& file, const std::string& folder) {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(file).parent_path();
    return std::filesystem::equivalent(dir.empty() ? std::filesystem::path(".") : dir, folder, ec) && !ec;
}

void open_with_desktop(QWidget* owner, const std::string& path) {
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(qs(path))))
        error_box(owner, "Cannot open " + path + ": no application is set up for it.");
}

QIcon glyph(const char* name) {
    QIcon icon;
    for (int size : {16, 24, 32, 48})
        icon.addFile(QStringLiteral(":/glyphs/%1-%2.png").arg(qs(name)).arg(size), QSize(size, size));
    return icon;
}

} // namespace em34xx::qt
