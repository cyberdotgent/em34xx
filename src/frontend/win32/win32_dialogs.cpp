// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/win32/win32_dialogs.hpp"

#include "frontend/win32/resource.h"
#include "em34xx_version.h"
#include "models/model.hpp"
#include "tn5250/tls_policy.hpp"
#include "util/fingerprint.hpp"
#include "util/strings.hpp"

#include <commctrl.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

namespace em34xx {
namespace win32 {

namespace {

HINSTANCE instance() { return GetModuleHandleW(nullptr); }

template <class T>
T* dialog_data(HWND dlg) {
    return reinterpret_cast<T*>(GetWindowLongPtrW(dlg, DWLP_USER));
}

template <class T>
T* init_dialog_data(HWND dlg, LPARAM lp) {
    SetWindowLongPtrW(dlg, DWLP_USER, lp);
    return reinterpret_cast<T*>(lp);
}

void combo_add(HWND dlg, int id, const std::string& text) {
    SendDlgItemMessageW(dlg, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(text).c_str()));
}

int combo_sel(HWND dlg, int id) { return int(SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0)); }
void combo_set_sel(HWND dlg, int id, int i) { SendDlgItemMessageW(dlg, id, CB_SETCURSEL, WPARAM(i), 0); }

void lv_add_column(HWND lv, int index, const wchar_t* title, int width_dlu_px) {
    LVCOLUMNW c;
    ZeroMemory(&c, sizeof c);
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = const_cast<wchar_t*>(title);
    c.cx = width_dlu_px;
    c.iSubItem = index;
    ListView_InsertColumn(lv, index, &c);
}

void lv_set(HWND lv, int row, int col, const std::string& text) {
    std::wstring w = widen(text);
    if (col == 0) {
        LVITEMW it;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = &w[0];
        ListView_InsertItem(lv, &it);
    } else {
        ListView_SetItemText(lv, row, col, &w[0]);
    }
}

int lv_selected(HWND lv) { return ListView_GetNextItem(lv, -1, LVNI_SELECTED); }

void lv_select(HWND lv, int row) {
    if (row < 0) return;
    ListView_SetItemState(lv, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(lv, row, FALSE);
}

// Dialog units -> pixels for a list column.
int dlu_x(HWND dlg, int dlu) {
    RECT r{0, 0, dlu, 0};
    MapDialogRect(dlg, &r);
    return r.right;
}

const char* subtype_label(DisplaySubtype d) {
    switch (d) {
    case DisplaySubtype::Green: return "Green";
    case DisplaySubtype::Amber: return "Amber";
    case DisplaySubtype::PaperWhite: return "Paper white";
    case DisplaySubtype::Color: return "Colour";
    }
    return "?";
}
const DisplaySubtype kSubtypes[] = {DisplaySubtype::Green, DisplaySubtype::Amber, DisplaySubtype::PaperWhite,
                                    DisplaySubtype::Color};
const unsigned kSpeeds[] = {25, 50, 100, 200, 400, 0};
const TlsVersion kTlsVersions[] = {TlsVersion::Ssl3, TlsVersion::Tls1_0, TlsVersion::Tls1_1, TlsVersion::Tls1_2,
                                   TlsVersion::Tls1_3};

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
    t += "Subject:\t" + c.subject + "\r\n";
    t += "Issuer:\t" + c.issuer + "\r\n";
    t += "Valid from:\t" + (c.valid_from.empty() ? std::string("?") : c.valid_from) + "\r\n";
    t += "Valid until:\t" + c.expires + (c.expired ? "  (EXPIRED)" : "") + "\r\n";
    t += "SHA-256:\t" + (c.have_digest ? format_sha256_fingerprint(c.sha256) : std::string("unavailable")) + "\r\n";
    t += c.chain_ok ? std::string("Verification:\tOK\r\n") : "Verification:\tfailed: " + c.chain_error + "\r\n";
    return t;
}

} // namespace

// ---------------------------------------------------------------------------
// ROM images

namespace {

struct RomDlg {
    const UserDirs* dirs;
    const Model* required;
    std::vector<RomStatusRow> rows;
};

void rom_fill(HWND dlg, RomDlg& d) {
    HWND lv = GetDlgItem(dlg, IDC_ROM_LIST);
    const int keep = lv_selected(lv);
    ListView_DeleteAllItems(lv);
    d.rows = rom_status(d.dirs->roms());
    if (d.required) {
        std::stable_sort(d.rows.begin(), d.rows.end(), [&](const RomStatusRow& a, const RomStatusRow& b) {
            return (a.model == d.required->info().name) > (b.model == d.required->info().name);
        });
    }
    int row = 0;
    for (const RomStatusRow& r : d.rows) {
        std::string state = rom_file_state_name(r.status.state);
        if (!r.status.note.empty()) state += " (" + r.status.note + ")";
        lv_set(lv, row, 0, "IBM " + r.model + (r.model_status == "bring-up" ? "" : " [" + r.model_status + "]"));
        lv_set(lv, row, 1, r.spec.file);
        lv_set(lv, row, 2, r.spec.label);
        lv_set(lv, row, 3, state);
        ++row;
    }
    lv_select(lv, std::min(keep, row - 1));
    std::string summary;
    for (const Model* m : runnable_models()) {
        const bool ready = model_roms_ready(*m, d.dirs->roms());
        summary += "IBM " + m->info().name + ": " + (ready ? "ready to run. " : "ROM images missing or damaged. ");
    }
    set_text(dlg, IDC_ROM_SUMMARY, summary);
}

INT_PTR CALLBACK rom_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        RomDlg* d = init_dialog_data<RomDlg>(dlg, lp);
        HWND lv = GetDlgItem(dlg, IDC_ROM_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        lv_add_column(lv, 0, L"Model", dlu_x(dlg, 70));
        lv_add_column(lv, 1, L"File", dlu_x(dlg, 50));
        lv_add_column(lv, 2, L"ROM", dlu_x(dlg, 130));
        lv_add_column(lv, 3, L"Status", dlu_x(dlg, 110));
        set_text(dlg, IDC_ROM_FOLDER, d->dirs->roms());
        if (d->required && !model_roms_ready(*d->required, d->dirs->roms()))
            SetDlgItemTextW(dlg, IDC_STATIC_TEXT, load_string(IDS_ROMS_MISSING).c_str());
        rom_fill(dlg, *d);
        SetFocus(GetDlgItem(dlg, d->required ? IDC_ROM_IMPORT : IDC_ROM_LIST));
        return FALSE;   // focus set
    }
    case WM_COMMAND: {
        RomDlg* d = dialog_data<RomDlg>(dlg);
        switch (LOWORD(wp)) {
        case IDC_ROM_IMPORT: {
            const auto files = open_files_dialog(dlg, L"Import ROM Images", load_string(IDS_FILTER_ROMS));
            std::string report;
            for (const std::string& f : files) {
                std::string stored, note, error;
                if (d->dirs->import_rom(f, stored, note, error)) report += stored + ": " + note + "\n";
                else report += error + "\n";
            }
            rom_fill(dlg, *d);
            if (!report.empty()) message_box(dlg, report, MB_OK | MB_ICONINFORMATION);
            return TRUE;
        }
        case IDC_ROM_REMOVE: {
            const int sel = lv_selected(GetDlgItem(dlg, IDC_ROM_LIST));
            if (sel < 0 || size_t(sel) >= d->rows.size()) return TRUE;
            const RomStatusRow& r = d->rows[size_t(sel)];
            if (r.status.state == RomFileStatus::State::Missing) return TRUE;
            if (message_box(dlg, "Remove " + r.spec.file + " from the ROM folder?", MB_YESNO | MB_ICONQUESTION) !=
                IDYES)
                return TRUE;
            std::string error;
            if (!d->dirs->remove_rom(r.spec.file, error)) error_box(dlg, error);
            rom_fill(dlg, *d);
            return TRUE;
        }
        case IDC_ROM_RECHECK:
            rom_fill(dlg, *d);
            return TRUE;
        case IDOK:
        case IDCANCEL:
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

} // namespace

bool rom_manager_dialog(HWND owner, const UserDirs& dirs, const Model* required) {
    RomDlg d{&dirs, required, {}};
    DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_ROMS), owner, rom_proc, reinterpret_cast<LPARAM>(&d));
    return !required || model_roms_ready(*required, dirs.roms());
}

// ---------------------------------------------------------------------------
// Settings

namespace {

struct SettingsDlg {
    const UserDirs* dirs;
    AppSettings s;
    bool restart = false;
};

INT_PTR CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SettingsDlg* d = init_dialog_data<SettingsDlg>(dlg, lp);
        for (size_t i = 0; i < 4; ++i) {
            combo_add(dlg, IDC_SET_SUBTYPE, subtype_label(kSubtypes[i]));
            if (kSubtypes[i] == d->s.display) combo_set_sel(dlg, IDC_SET_SUBTYPE, int(i));
        }
        CheckDlgButton(dlg, IDC_SET_CRT, d->s.crt ? BST_CHECKED : BST_UNCHECKED);
        int speed_sel = 2;
        for (size_t i = 0; i < sizeof kSpeeds / sizeof kSpeeds[0]; ++i) {
            combo_add(dlg, IDC_SET_SPEED, kSpeeds[i] ? format("%u %%", kSpeeds[i]) : std::string("Unthrottled"));
            if (kSpeeds[i] == d->s.speed_percent) speed_sel = int(i);
        }
        combo_set_sel(dlg, IDC_SET_SPEED, speed_sel);
        for (size_t i = 0; i < 5; ++i) {
            combo_add(dlg, IDC_SET_TLSMIN, tls_version_label(kTlsVersions[i]));
            if (kTlsVersions[i] == d->s.tls_min_version) combo_set_sel(dlg, IDC_SET_TLSMIN, int(i));
        }
        set_text(dlg, IDC_SET_FOLDER, "Settings, ROMs, keymap and profiles are kept in " + d->dirs->root());
        return TRUE;
    }
    case WM_COMMAND: {
        SettingsDlg* d = dialog_data<SettingsDlg>(dlg);
        switch (LOWORD(wp)) {
        case IDC_SET_ROMS:
            rom_manager_dialog(dlg, *d->dirs, nullptr);
            return TRUE;
        case IDC_SET_ASSOC: {
            if (MessageBoxW(dlg, load_string(IDS_ASSOC_CONFIRM).c_str(), load_string(IDS_APP_TITLE).c_str(),
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return TRUE;
            std::string error;
            if (register_file_association(error))
                MessageBoxW(dlg, load_string(IDS_ASSOC_DONE).c_str(), load_string(IDS_APP_TITLE).c_str(),
                            MB_OK | MB_ICONINFORMATION);
            else
                error_box(dlg, error);
            return TRUE;
        }
        case IDOK: {
            const int st = combo_sel(dlg, IDC_SET_SUBTYPE);
            if (st >= 0 && st < 4 && kSubtypes[st] != d->s.display) {
                d->s.display = kSubtypes[st];
                d->restart = true;
            }
            d->s.crt = IsDlgButtonChecked(dlg, IDC_SET_CRT) == BST_CHECKED;
            const int sp = combo_sel(dlg, IDC_SET_SPEED);
            if (sp >= 0 && sp < int(sizeof kSpeeds / sizeof kSpeeds[0])) d->s.speed_percent = kSpeeds[sp];
            const int tv = combo_sel(dlg, IDC_SET_TLSMIN);
            if (tv >= 0 && tv < 5) d->s.tls_min_version = kTlsVersions[tv];
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

} // namespace

bool settings_dialog(HWND owner, const UserDirs& dirs, AppSettings& s, bool& restart_needed) {
    SettingsDlg d{&dirs, s, false};
    if (DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_SETTINGS), owner, settings_proc,
                        reinterpret_cast<LPARAM>(&d)) != IDOK)
        return false;
    s = d.s;
    restart_needed = d.restart;
    return true;
}

// ---------------------------------------------------------------------------
// Name prompt

namespace {
struct NameDlg {
    std::wstring title, prompt;
    std::string name;
};

INT_PTR CALLBACK name_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        NameDlg* d = init_dialog_data<NameDlg>(dlg, lp);
        SetWindowTextW(dlg, d->title.c_str());
        SetDlgItemTextW(dlg, IDC_NAME_PROMPT, d->prompt.c_str());
        set_text(dlg, IDC_NAME_EDIT, d->name);
        SendDlgItemMessageW(dlg, IDC_NAME_EDIT, EM_SETSEL, 0, -1);
        return TRUE;
    }
    case WM_COMMAND: {
        NameDlg* d = dialog_data<NameDlg>(dlg);
        if (LOWORD(wp) == IDOK) {
            const std::string name = trim(get_text(dlg, IDC_NAME_EDIT));
            std::string why;
            if (!UserDirs::valid_profile_name(name, why)) {
                error_box(dlg, why);
                return TRUE;
            }
            d->name = name;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}
} // namespace

bool name_dialog(HWND owner, const std::wstring& title, const std::wstring& prompt, std::string& name) {
    NameDlg d{title, prompt, name};
    if (DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_NAME), owner, name_proc, reinterpret_cast<LPARAM>(&d)) !=
        IDOK)
        return false;
    name = d.name;
    return true;
}

// ---------------------------------------------------------------------------
// TLS settings

namespace {

constexpr UINT WM_APP_FETCHED = WM_APP + 1;

// A certificate fetch runs on its own thread; the dialog may close first.
struct FetchJob {
    std::mutex mu;
    HWND target = nullptr;   // null once the dialog is gone
    bool ok = false;
    tn5250::PeerCertificate peer;
    std::string error;
};

struct TlsDlg {
    Profile p;
    const AppSettings* s;
    std::shared_ptr<FetchJob> job;
};

void tls_enable(HWND dlg) {
    const bool hash = IsDlgButtonChecked(dlg, IDC_TLS_HASH) == BST_CHECKED;
    EnableWindow(GetDlgItem(dlg, IDC_TLS_HASH_EDIT), hash);
}

INT_PTR CALLBACK tls_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        TlsDlg* d = init_dialog_data<TlsDlg>(dlg, lp);
        const int radio = d->p.tls_mode == ProfileTlsMode::System      ? IDC_TLS_SYSTEM
                          : d->p.tls_mode == ProfileTlsMode::AcceptAll ? IDC_TLS_ANY
                                                                       : IDC_TLS_HASH;
        CheckRadioButton(dlg, IDC_TLS_SYSTEM, IDC_TLS_HASH, radio);
        set_text(dlg, IDC_TLS_HASH_EDIT, d->p.tls_cert_sha256);
        const ProfileSession& s1 = d->p.sessions[0];
        set_text(dlg, IDC_TLS_FETCH_HOST,
                 s1.used() ? format("from %s:%u (session 1)", s1.host.c_str(), s1.port)
                           : std::string("enter the session 1 host first"));
        EnableWindow(GetDlgItem(dlg, IDC_TLS_FETCH), s1.used());
        combo_add(dlg, IDC_TLS_MINVER, "Default (" + tls_version_label(d->s->tls_min_version) + ")");
        combo_set_sel(dlg, IDC_TLS_MINVER, 0);
        for (size_t i = 0; i < 5; ++i) {
            combo_add(dlg, IDC_TLS_MINVER, tls_version_label(kTlsVersions[i]));
            TlsVersion v;
            if (!d->p.tls_min_version.empty() && parse_tls_version(d->p.tls_min_version, v) && v == kTlsVersions[i])
                combo_set_sel(dlg, IDC_TLS_MINVER, int(i + 1));
        }
        const UINT tabs = 44;   // dialog units
        SendDlgItemMessageW(dlg, IDC_TLS_CERT, EM_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(&tabs));
        tls_enable(dlg);
        return TRUE;
    }
    case WM_APP_FETCHED: {
        TlsDlg* d = dialog_data<TlsDlg>(dlg);
        if (!d->job) return TRUE;
        std::lock_guard<std::mutex> g(d->job->mu);
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        EnableWindow(GetDlgItem(dlg, IDC_TLS_FETCH), TRUE);
        if (d->job->ok) {
            set_text(dlg, IDC_TLS_CERT, certificate_text(d->job->peer));
            if (d->job->peer.have_digest) {
                set_text(dlg, IDC_TLS_HASH_EDIT, format_sha256_fingerprint(d->job->peer.sha256));
                CheckRadioButton(dlg, IDC_TLS_SYSTEM, IDC_TLS_HASH, IDC_TLS_HASH);
                tls_enable(dlg);
            }
        } else {
            set_text(dlg, IDC_TLS_CERT, "Could not fetch the certificate:\r\n" + d->job->error);
        }
        return TRUE;
    }
    case WM_DESTROY: {
        TlsDlg* d = dialog_data<TlsDlg>(dlg);
        if (d && d->job) {
            std::lock_guard<std::mutex> g(d->job->mu);
            d->job->target = nullptr;
        }
        break;
    }
    case WM_COMMAND: {
        TlsDlg* d = dialog_data<TlsDlg>(dlg);
        switch (LOWORD(wp)) {
        case IDC_TLS_SYSTEM:
        case IDC_TLS_ANY:
        case IDC_TLS_HASH:
            tls_enable(dlg);
            return TRUE;
        case IDC_TLS_FETCH: {
            if (d->job) {
                std::lock_guard<std::mutex> g(d->job->mu);
                d->job->target = nullptr;   // forget an earlier fetch still running
            }
            auto job = std::make_shared<FetchJob>();
            job->target = dlg;
            d->job = job;
            Tn5250Config cfg;
            cfg.host = d->p.sessions[0].host;
            cfg.port = d->p.sessions[0].port;
            cfg.tls = true;
            cfg.tls_min_version = d->s->tls_min_version;
            const int mv = combo_sel(dlg, IDC_TLS_MINVER);
            if (mv > 0) cfg.tls_min_version = kTlsVersions[mv - 1];
            EnableWindow(GetDlgItem(dlg, IDC_TLS_FETCH), FALSE);
            set_text(dlg, IDC_TLS_CERT, format("Connecting to %s:%u...", cfg.host.c_str(), cfg.port));
            std::thread([job, cfg] {
                tn5250::PeerCertificate peer;
                std::string error;
                const bool ok = tn5250::tls_fetch_certificate(cfg, peer, error);
                std::lock_guard<std::mutex> g(job->mu);
                job->ok = ok;
                job->peer = peer;
                job->error = error;
                if (job->target) PostMessageW(job->target, WM_APP_FETCHED, 0, 0);
            }).detach();
            return TRUE;
        }
        case IDOK: {
            Profile p = d->p;
            p.tls_mode = IsDlgButtonChecked(dlg, IDC_TLS_ANY) == BST_CHECKED    ? ProfileTlsMode::AcceptAll
                         : IsDlgButtonChecked(dlg, IDC_TLS_HASH) == BST_CHECKED ? ProfileTlsMode::Fingerprint
                                                                                : ProfileTlsMode::System;
            p.tls_cert_sha256 = trim(get_text(dlg, IDC_TLS_HASH_EDIT));
            Sha256Digest digest;
            if (!p.tls_cert_sha256.empty() && parse_sha256_fingerprint(p.tls_cert_sha256, digest))
                p.tls_cert_sha256 = format_sha256_fingerprint(digest);   // canonical form
            if (p.tls_mode == ProfileTlsMode::Fingerprint && !parse_sha256_fingerprint(p.tls_cert_sha256, digest)) {
                error_box(dlg, "The certificate hash must be a SHA-256 fingerprint: 32 hex bytes, colons optional. "
                               "Use Fetch to read it from the server.");
                return TRUE;
            }
            const int mv = combo_sel(dlg, IDC_TLS_MINVER);
            p.tls_min_version = mv > 0 ? tls_version_name(kTlsVersions[mv - 1]) : "";
            d->p = p;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

} // namespace

bool tls_settings_dialog(HWND owner, Profile& p, const AppSettings& s) {
    TlsDlg d{p, &s, nullptr};
    if (DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_TLS), owner, tls_proc, reinterpret_cast<LPARAM>(&d)) != IDOK)
        return false;
    p = d.p;
    return true;
}

// ---------------------------------------------------------------------------
// Profile editor

namespace {

struct ProfileDlg {
    Profile p;
    ProfileEditMode mode;
    const AppSettings* s;
    std::string title;
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

// Read the fields into d.p; false (with a message) when a port is invalid.
bool profile_read(HWND dlg, ProfileDlg& d, bool quiet) {
    std::vector<const Model*> models = runnable_models();
    const int m = combo_sel(dlg, IDC_PE_MODEL);
    if (m >= 0 && size_t(m) < models.size()) d.p.model = models[size_t(m)]->info().name;
    const int host_ids[2] = {IDC_PE_HOST1, IDC_PE_HOST2};
    const int port_ids[2] = {IDC_PE_PORT1, IDC_PE_PORT2};
    const int dev_ids[2] = {IDC_PE_DEV1, IDC_PE_DEV2};
    for (size_t i = 0; i < 2; ++i) {
        ProfileSession& s = d.p.sessions[i];
        s.host = trim(get_text(dlg, host_ids[i]));
        s.device_name = trim(get_text(dlg, dev_ids[i]));
        const std::string port = trim(get_text(dlg, port_ids[i]));
        uint64_t v = 0;
        if (port.empty()) v = d.p.tls ? 992 : 23;
        else if (!parse_uint(port, v) || v == 0 || v > 65535) {
            if (!quiet) error_box(dlg, format("Session %zu: the port must be a number from 1 to 65535.", i + 1));
            return false;
        }
        s.port = uint16_t(v);
    }
    d.p.tls = IsDlgButtonChecked(dlg, IDC_PE_TLS) == BST_CHECKED;
    return true;
}

void profile_update_tls(HWND dlg, ProfileDlg& d) {
    const bool on = IsDlgButtonChecked(dlg, IDC_PE_TLS) == BST_CHECKED;
    EnableWindow(GetDlgItem(dlg, IDC_PE_TLS_SETTINGS), on);
    set_text(dlg, IDC_PE_TLS_SUMMARY, on ? tls_summary(d.p, *d.s) : std::string("TLS off: plain telnet"));
}

INT_PTR CALLBACK profile_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        ProfileDlg* d = init_dialog_data<ProfileDlg>(dlg, lp);
        SetWindowTextW(dlg, widen(d->title).c_str());
        SetDlgItemTextW(dlg, IDOK, d->mode == ProfileEditMode::Connect ? L"Connect" : L"Save");
        int i = 0;
        for (const Model* m : runnable_models()) {
            combo_add(dlg, IDC_PE_MODEL, "IBM " + m->info().name);
            if (m->info().name == d->p.model || i == 0) combo_set_sel(dlg, IDC_PE_MODEL, i);
            ++i;
        }
        const int host_ids[2] = {IDC_PE_HOST1, IDC_PE_HOST2};
        const int port_ids[2] = {IDC_PE_PORT1, IDC_PE_PORT2};
        const int dev_ids[2] = {IDC_PE_DEV1, IDC_PE_DEV2};
        for (size_t s = 0; s < 2; ++s) {
            const ProfileSession& ps = d->p.sessions[s];
            set_text(dlg, host_ids[s], ps.host);
            set_text(dlg, port_ids[s], ps.used() || s == 0 ? std::to_string(ps.port) : std::string());
            set_text(dlg, dev_ids[s], ps.device_name);
            SendDlgItemMessageW(dlg, dev_ids[s], EM_LIMITTEXT, 10, 0);
            SendDlgItemMessageW(dlg, port_ids[s], EM_LIMITTEXT, 5, 0);
        }
        CheckDlgButton(dlg, IDC_PE_TLS, d->p.tls ? BST_CHECKED : BST_UNCHECKED);
        profile_update_tls(dlg, *d);
        return TRUE;
    }
    case WM_COMMAND: {
        ProfileDlg* d = dialog_data<ProfileDlg>(dlg);
        switch (LOWORD(wp)) {
        case IDC_PE_TLS: {
            // Switch the conventional ports along with TLS.
            const bool on = IsDlgButtonChecked(dlg, IDC_PE_TLS) == BST_CHECKED;
            for (int id : {IDC_PE_PORT1, IDC_PE_PORT2}) {
                const std::string port = trim(get_text(dlg, id));
                if (port == (on ? "23" : "992")) set_text(dlg, id, on ? "992" : "23");
            }
            profile_update_tls(dlg, *d);
            return TRUE;
        }
        case IDC_PE_TLS_SETTINGS:
            profile_read(dlg, *d, true);
            if (tls_settings_dialog(dlg, d->p, *d->s)) profile_update_tls(dlg, *d);
            return TRUE;
        case IDOK: {
            if (!profile_read(dlg, *d, false)) return TRUE;
            const std::string why = d->p.validate();
            if (!why.empty()) {
                error_box(dlg, why);
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

} // namespace

bool profile_editor_dialog(HWND owner, Profile& p, ProfileEditMode mode, const AppSettings& s,
                           const std::string& title) {
    ProfileDlg d{p, mode, &s, title};
    if (DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_PROFILE), owner, profile_proc,
                        reinterpret_cast<LPARAM>(&d)) != IDOK)
        return false;
    p = d.p;
    return true;
}

// ---------------------------------------------------------------------------
// Profile manager

namespace {

struct ManagerDlg {
    const UserDirs* dirs;
    const AppSettings* s;
    std::vector<std::string> names;
    std::string chosen;
};

void manager_fill(HWND dlg, ManagerDlg& d, const std::string& select) {
    HWND lb = GetDlgItem(dlg, IDC_PROF_LIST);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    d.names = d.dirs->list_profiles();
    int sel = d.names.empty() ? -1 : 0;
    for (size_t i = 0; i < d.names.size(); ++i) {
        SendMessageW(lb, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(d.names[i]).c_str()));
        if (d.names[i] == select) sel = int(i);
    }
    SendMessageW(lb, LB_SETCURSEL, WPARAM(sel), 0);
    for (int id : {IDC_PROF_CONNECT, IDC_PROF_EDIT, IDC_PROF_RENAME, IDC_PROF_COPY, IDC_PROF_DELETE})
        EnableWindow(GetDlgItem(dlg, id), sel >= 0);
}

std::string manager_selected(HWND dlg, const ManagerDlg& d) {
    const LRESULT i = SendDlgItemMessageW(dlg, IDC_PROF_LIST, LB_GETCURSEL, 0, 0);
    return i >= 0 && size_t(i) < d.names.size() ? d.names[size_t(i)] : std::string();
}

INT_PTR CALLBACK manager_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        ManagerDlg* d = init_dialog_data<ManagerDlg>(dlg, lp);
        manager_fill(dlg, *d, std::string());
        return TRUE;
    }
    case WM_COMMAND: {
        ManagerDlg* d = dialog_data<ManagerDlg>(dlg);
        const std::string sel = manager_selected(dlg, *d);
        std::string error;
        switch (LOWORD(wp)) {
        case IDC_PROF_LIST:
            if (HIWORD(wp) == LBN_DBLCLK && !sel.empty()) {
                d->chosen = sel;
                EndDialog(dlg, IDOK);
            }
            return TRUE;
        case IDC_PROF_CONNECT:
            if (sel.empty()) return TRUE;
            d->chosen = sel;
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDC_PROF_NEW: {
            std::string name = d->dirs->unique_profile_name("New Profile");
            if (!name_dialog(dlg, L"New Profile", L"&Name of the new profile:", name)) return TRUE;
            if (d->dirs->profile_exists(name)) {
                error_box(dlg, "A profile named \"" + name + "\" already exists.");
                return TRUE;
            }
            Profile p;
            if (!profile_editor_dialog(dlg, p, ProfileEditMode::Save, *d->s, "New Profile: " + name)) return TRUE;
            if (!save_profile(d->dirs->profile_path(name), p, error)) error_box(dlg, error);
            manager_fill(dlg, *d, name);
            return TRUE;
        }
        case IDC_PROF_EDIT: {
            if (sel.empty()) return TRUE;
            Profile p;
            if (!load_profile(d->dirs->profile_path(sel), p, error)) {
                error_box(dlg, error);
                return TRUE;
            }
            if (!profile_editor_dialog(dlg, p, ProfileEditMode::Save, *d->s, "Profile: " + sel)) return TRUE;
            if (!save_profile(d->dirs->profile_path(sel), p, error)) error_box(dlg, error);
            return TRUE;
        }
        case IDC_PROF_RENAME: {
            if (sel.empty()) return TRUE;
            std::string name = sel;
            if (!name_dialog(dlg, L"Rename Profile", L"&New name:", name) || name == sel) return TRUE;
            if (!d->dirs->rename_profile(sel, name, error)) error_box(dlg, error);
            manager_fill(dlg, *d, name);
            return TRUE;
        }
        case IDC_PROF_COPY: {
            if (sel.empty()) return TRUE;
            const std::string name = d->dirs->unique_profile_name(sel + " (copy)");
            if (!d->dirs->copy_profile(sel, name, error)) error_box(dlg, error);
            manager_fill(dlg, *d, name);
            return TRUE;
        }
        case IDC_PROF_DELETE: {
            if (sel.empty()) return TRUE;
            if (message_box(dlg, string_with(IDS_DELETE_CONFIRM, sel), MB_YESNO | MB_ICONQUESTION) != IDYES)
                return TRUE;
            if (!d->dirs->delete_profile(sel, error)) error_box(dlg, error);
            manager_fill(dlg, *d, std::string());
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

} // namespace

std::string profile_manager_dialog(HWND owner, const UserDirs& dirs, const AppSettings& s) {
    ManagerDlg d{&dirs, &s, {}, {}};
    if (DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_PROFILES), owner, manager_proc,
                        reinterpret_cast<LPARAM>(&d)) != IDOK)
        return std::string();
    return d.chosen;
}

// ---------------------------------------------------------------------------
// Trust prompt

namespace {
struct TrustDlg {
    const tn5250::TlsTrustRequest* req;
    std::string profile;
};

INT_PTR CALLBACK trust_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        TrustDlg* d = init_dialog_data<TrustDlg>(dlg, lp);
        const tn5250::TlsTrustRequest& r = *d->req;
        SendDlgItemMessageW(dlg, IDC_TRUST_ICON, STM_SETICON,
                            reinterpret_cast<WPARAM>(LoadIconW(nullptr, MAKEINTRESOURCEW(32515) /* IDI_WARNING */)),
                            0);
        std::string text = format("The server certificate of %s:%u (session %u) is not trusted", r.host.c_str(),
                                  r.port, r.station + 1);
        text += r.problem == tn5250::TlsTrustProblem::PinMismatch
                    ? ": it is not the certificate this profile trusts. The server may have a new certificate, or "
                      "someone may be intercepting the connection."
                    : ". Someone may be intercepting the connection; connect only if you know this certificate.";
        set_text(dlg, IDC_TRUST_TEXT, text);
        std::string details = "Reason:\t" + r.reason + "\r\n\r\n" + certificate_text(r.peer);
        details += "\r\nTrust Permanently stores the SHA-256 in the profile \"" + d->profile +
                   "\"; only this certificate is accepted afterwards.";
        const UINT tabs = 44;   // dialog units
        SendDlgItemMessageW(dlg, IDC_TRUST_DETAILS, EM_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(&tabs));
        set_text(dlg, IDC_TRUST_DETAILS, details);
        SetFocus(GetDlgItem(dlg, IDCANCEL));
        MessageBeep(MB_ICONWARNING);
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TRUST_ONCE:
            EndDialog(dlg, int(tn5250::TlsTrustDecision::AcceptOnce) + 100);
            return TRUE;
        case IDC_TRUST_ALWAYS:
            EndDialog(dlg, int(tn5250::TlsTrustDecision::AcceptPermanently) + 100);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, int(tn5250::TlsTrustDecision::Reject) + 100);
            return TRUE;
        }
        break;
    }
    return FALSE;
}
} // namespace

tn5250::TlsTrustDecision trust_dialog(HWND owner, const tn5250::TlsTrustRequest& req, const std::string& profile) {
    TrustDlg d{&req, profile};
    const INT_PTR r =
        DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_TRUST), owner, trust_proc, reinterpret_cast<LPARAM>(&d));
    if (r == int(tn5250::TlsTrustDecision::AcceptOnce) + 100) return tn5250::TlsTrustDecision::AcceptOnce;
    if (r == int(tn5250::TlsTrustDecision::AcceptPermanently) + 100) return tn5250::TlsTrustDecision::AcceptPermanently;
    return tn5250::TlsTrustDecision::Reject;
}

// ---------------------------------------------------------------------------
// Keymap editor

namespace {

struct EntryDlg {
    KeymapEntry e;
    std::string original_mods;
    bool orig_shift = false, orig_ctrl = false, orig_alt = false, orig_any = false;
};

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

constexpr UINT_PTR kCaptureSubclass = 1;

LRESULT CALLBACK capture_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const uint32_t hid = hid_from_key_message(wp, lp);
        if (!hid) return 0;
        HWND dlg = GetParent(hwnd);
        const bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0,
                   alt = GetKeyState(VK_MENU) < 0;
        const uint8_t own = host_modifier_bit(hid);
        // A modifier pressed on its own is the key itself, without modifiers.
        const bool m_shift = shift && !(own & (kModLShift | kModRShift));
        const bool m_ctrl = ctrl && !(own & (kModLCtrl | kModRCtrl));
        const bool m_alt = alt && !(own & (kModLAlt | kModRAlt));
        const std::string name = host_key_name(hid);
        SetDlgItemTextW(dlg, IDC_KE_HOST, widen(name).c_str());
        CheckDlgButton(dlg, IDC_KE_SHIFT, m_shift ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_KE_CTRL, m_ctrl ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_KE_ALT, m_alt ? BST_CHECKED : BST_UNCHECKED);
        std::string shown = (m_ctrl ? "Ctrl+" : "") + std::string(m_alt ? "Alt+" : "") + (m_shift ? "Shift+" : "") + name;
        SetWindowTextW(hwnd, widen(shown).c_str());
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, capture_proc, kCaptureSubclass);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

INT_PTR CALLBACK entry_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        EntryDlg* d = init_dialog_data<EntryDlg>(dlg, lp);
        SetWindowSubclass(GetDlgItem(dlg, IDC_KE_CAPTURE), capture_proc, kCaptureSubclass, 0);
        SetDlgItemTextW(dlg, IDC_KE_CAPTURE, L"(click here, then press a key)");
        for (uint32_t hid = 0x04; hid <= 0xE7; ++hid) {
            const std::string n = host_key_name(hid);
            if (n.rfind("0x", 0) != 0) combo_add(dlg, IDC_KE_HOST, n);
        }
        set_text(dlg, IDC_KE_HOST, d->e.host);
        mods_flags(d->e.mods, d->orig_shift, d->orig_ctrl, d->orig_alt, d->orig_any);
        d->original_mods = d->e.mods;
        CheckDlgButton(dlg, IDC_KE_SHIFT, d->orig_shift ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_KE_CTRL, d->orig_ctrl ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_KE_ALT, d->orig_alt ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_KE_ANY, d->orig_any ? BST_CHECKED : BST_UNCHECKED);
        combo_add(dlg, IDC_KE_TO, "none");
        combo_add(dlg, IDC_KE_TO, "positional");
        for (const std::string& f : terminal_function_names()) combo_add(dlg, IDC_KE_TO, f);
        set_text(dlg, IDC_KE_TO, d->e.to);
        for (int i = 0; i < 3; ++i) {
            combo_add(dlg, IDC_KE_KEYBOARD, kKeyboards[i]);
            if (to_lower(d->e.keyboard) == kKeyboardValues[i] || (i == 0 && to_lower(d->e.keyboard) == "both"))
                combo_set_sel(dlg, IDC_KE_KEYBOARD, i);
        }
        if (combo_sel(dlg, IDC_KE_KEYBOARD) < 0) combo_set_sel(dlg, IDC_KE_KEYBOARD, 0);
        CheckDlgButton(dlg, IDC_KE_TAP, d->e.tap ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    case WM_COMMAND: {
        EntryDlg* d = dialog_data<EntryDlg>(dlg);
        if (LOWORD(wp) == IDOK) {
            KeymapEntry e = d->e;
            e.host = trim(get_text(dlg, IDC_KE_HOST));
            e.to = trim(get_text(dlg, IDC_KE_TO));
            const bool shift = IsDlgButtonChecked(dlg, IDC_KE_SHIFT) == BST_CHECKED;
            const bool ctrl = IsDlgButtonChecked(dlg, IDC_KE_CTRL) == BST_CHECKED;
            const bool alt = IsDlgButtonChecked(dlg, IDC_KE_ALT) == BST_CHECKED;
            const bool any = IsDlgButtonChecked(dlg, IDC_KE_ANY) == BST_CHECKED;
            if (shift == d->orig_shift && ctrl == d->orig_ctrl && alt == d->orig_alt && any == d->orig_any) {
                e.mods = d->original_mods;   // keep left/right detail the boxes cannot show
            } else {
                std::string m;
                if (shift) m += "shift ";
                if (ctrl) m += "ctrl ";
                if (alt) m += "alt ";
                if (any) m += "*";
                e.mods = trim(m);
            }
            const int kb = combo_sel(dlg, IDC_KE_KEYBOARD);
            e.keyboard = kb > 0 && kb < 3 ? kKeyboardValues[kb] : "";
            e.tap = IsDlgButtonChecked(dlg, IDC_KE_TAP) == BST_CHECKED;
            std::string error;
            if (!check_keymap_entry(e, error)) {
                error_box(dlg, error);
                return TRUE;
            }
            d->e = e;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

bool entry_dialog(HWND owner, KeymapEntry& e) {
    EntryDlg d;
    d.e = e;
    if (DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_KEYMAP_ENTRY), owner, entry_proc,
                        reinterpret_cast<LPARAM>(&d)) != IDOK)
        return false;
    e = d.e;
    return true;
}

struct KeymapDlg {
    KeymapFile file;
};

void keymap_fill(HWND dlg, KeymapDlg& d, int select) {
    HWND lv = GetDlgItem(dlg, IDC_KM_LIST);
    ListView_DeleteAllItems(lv);
    int row = 0;
    for (const KeymapEntry& e : d.file.entries) {
        lv_set(lv, row, 0, e.host);
        lv_set(lv, row, 1, e.mods.empty() ? std::string("-") : e.mods == "*" ? std::string("any") : e.mods);
        lv_set(lv, row, 2, e.to);
        lv_set(lv, row, 3, e.keyboard.empty() ? std::string("both") : e.keyboard);
        std::string opt;
        if (e.tap) opt += "tap ";
        if (!e.shift.empty()) opt += "shift " + e.shift + " ";
        if (!e.alt.empty()) opt += "alt " + e.alt;
        lv_set(lv, row, 4, trim(opt));
        ++row;
    }
    lv_select(lv, std::min(select, row - 1));
    std::string base;
    if (d.file.base == "positional")
        base = "Keys not listed press the terminal key at the same position; Shift+F1..F12 enter F13..F24 on the "
               "122-key keyboard.";
    else if (d.file.base == "pcomm")
        base = "Keys not listed follow the built-in PCOMM layout (base \"pcomm\").";
    else
        base = "Keys not listed press the terminal key at the same position.";
    set_text(dlg, IDC_KM_BASE, base);
}

INT_PTR CALLBACK keymap_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        KeymapDlg* d = init_dialog_data<KeymapDlg>(dlg, lp);
        HWND lv = GetDlgItem(dlg, IDC_KM_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        lv_add_column(lv, 0, L"PC key", dlu_x(dlg, 60));
        lv_add_column(lv, 1, L"Modifiers", dlu_x(dlg, 48));
        lv_add_column(lv, 2, L"Terminal key", dlu_x(dlg, 58));
        lv_add_column(lv, 3, L"Keyboard", dlu_x(dlg, 42));
        lv_add_column(lv, 4, L"Options", dlu_x(dlg, 70));
        keymap_fill(dlg, *d, 0);
        return TRUE;
    }
    case WM_NOTIFY: {
        const NMHDR* n = reinterpret_cast<const NMHDR*>(lp);
        if (n->idFrom == IDC_KM_LIST && n->code == NM_DBLCLK) {
            SendMessageW(dlg, WM_COMMAND, IDC_KM_EDIT, 0);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        KeymapDlg* d = dialog_data<KeymapDlg>(dlg);
        const int sel = lv_selected(GetDlgItem(dlg, IDC_KM_LIST));
        switch (LOWORD(wp)) {
        case IDC_KM_ADD: {
            KeymapEntry e;
            e.host = "F1";
            e.to = "f1";
            if (!entry_dialog(dlg, e)) return TRUE;
            d->file.entries.push_back(e);
            keymap_fill(dlg, *d, int(d->file.entries.size()) - 1);
            return TRUE;
        }
        case IDC_KM_EDIT:
            if (sel < 0 || size_t(sel) >= d->file.entries.size()) return TRUE;
            if (entry_dialog(dlg, d->file.entries[size_t(sel)])) keymap_fill(dlg, *d, sel);
            return TRUE;
        case IDC_KM_REMOVE:
            if (sel < 0 || size_t(sel) >= d->file.entries.size()) return TRUE;
            d->file.entries.erase(d->file.entries.begin() + sel);
            keymap_fill(dlg, *d, sel);
            return TRUE;
        case IDC_KM_RESET:
            if (message_box(dlg, "Replace the keymap with the default IBM Personal Communications (PCOMM) layout?",
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return TRUE;
            d->file = pcomm_keymap_file();
            keymap_fill(dlg, *d, 0);
            return TRUE;
        case IDOK:
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

} // namespace

bool keymap_dialog(HWND owner, KeymapFile& file) {
    KeymapDlg d{file};
    if (DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_KEYMAP), owner, keymap_proc, reinterpret_cast<LPARAM>(&d)) !=
        IDOK)
        return false;
    file = d.file;
    return true;
}

// ---------------------------------------------------------------------------
// About

namespace {
// The third-party notices, embedded as RCDATA (res/notices.txt, UTF-8 with
// LF line ends), converted for a multi-line edit control.
std::wstring load_notices() {
    HRSRC r = FindResourceW(instance(), MAKEINTRESOURCEW(IDR_NOTICES), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return L"(notices not found)";
    HGLOBAL g = LoadResource(instance(), r);
    const char* data = g ? static_cast<const char*>(LockResource(g)) : nullptr;
    if (!data) return L"(notices not found)";
    std::string text(data, SizeofResource(instance(), r));
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 32);
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    return widen(crlf);
}

INT_PTR CALLBACK about_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        const UserDirs* dirs = reinterpret_cast<const UserDirs*>(lp);
        set_text(dlg, IDC_ABOUT_TEXT,
                 std::string("em34xxw version ") + EM34XX_VERSION_STRING +
                     "\r\nHardware emulator for IBM 34xx twinax terminals, connected to IBM i over "
                     "TN5250.\r\nFree software under the GNU GPL, version 3 or later; no warranty."
                     "\r\n\r\nUser folder: " +
                     dirs->root() + "\r\nTLS: " + (tn5250::tls_backend_name() ? tn5250::tls_backend_name() : "none"));
        const std::string url = EM34XX_HOMEPAGE_URL;
        set_text(dlg, IDC_ABOUT_LINK,
                 url.empty() ? std::string("Project page: to be announced")
                             : "Project page: <a href=\"" + url + "\">" + url + "</a>");
        SetDlgItemTextW(dlg, IDC_ABOUT_NOTICES, load_notices().c_str());
        return TRUE;
    }
    case WM_NOTIFY: {
        const NMHDR* h = reinterpret_cast<const NMHDR*>(lp);
        if (h->idFrom == IDC_ABOUT_LINK && (h->code == NM_CLICK || h->code == NM_RETURN)) {
            const NMLINK* link = reinterpret_cast<const NMLINK*>(lp);
            if (link->item.szUrl[0])
                ShellExecuteW(dlg, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    }
    return FALSE;
}
} // namespace

void about_dialog(HWND owner, const UserDirs& dirs) {
    DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_ABOUT), owner, about_proc,
                    reinterpret_cast<LPARAM>(const_cast<UserDirs*>(&dirs)));
}

// ---------------------------------------------------------------------------
// File association

namespace {
bool set_reg(const std::wstring& key, const std::wstring& value, std::string& error) {
    HKEY h = nullptr;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr);
    if (r == ERROR_SUCCESS) {
        r = RegSetValueExW(h, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                           DWORD((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
    }
    if (r != ERROR_SUCCESS) {
        error = "cannot write HKEY_CURRENT_USER\\" + narrow(key) + format(" (error %ld)", long(r));
        return false;
    }
    return true;
}
} // namespace

bool register_file_association(std::string& error) {
    const std::wstring exe = widen(module_path());
    const std::wstring base = L"Software\\Classes\\";
    const std::wstring prog = L"em34xx.Profile";
    if (!set_reg(base + L".em34xx", prog, error)) return false;
    if (!set_reg(base + prog, load_string(IDS_FILE_TYPE_NAME), error)) return false;
    if (!set_reg(base + prog + L"\\DefaultIcon", L"\"" + exe + L"\",-" + std::to_wstring(IDI_PROFILE), error))
        return false;
    if (!set_reg(base + prog + L"\\shell\\open\\command", L"\"" + exe + L"\" \"%1\"", error)) return false;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

} // namespace win32
} // namespace em34xx
