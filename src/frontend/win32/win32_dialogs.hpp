// SPDX-License-Identifier: GPL-3.0-or-later
// em34xxw dialogs (templates in em34xxw.rc): ROM images, settings, profile
// manager and editor, TLS settings, the TLS trust prompt, the invalid
// certificate prompt, the keymap editor and About. All modal; each returns once the user closes it.
#pragma once

#include "config/profile.hpp"
#include "config/settings.hpp"
#include "config/user_dirs.hpp"
#include "frontend/win32/win32_util.hpp"
#include "input/host_keymap.hpp"
#include "tn5250/tls_trust.hpp"

#include <string>

namespace em34xx {

class Model;

namespace win32 {

// ROM images: what each model needs and its state; import, remove,
// re-check. `required`: the model about to run (its rows come first, the
// summary says whether it is ready). `need_any`: no model can run yet and
// one must (first start). True when `required` is ready at close; without
// one, when some model is ready (need_any) or always.
bool rom_manager_dialog(HWND owner, const UserDirs& dirs, const Model* required, bool need_any = false);

// True when `model` has its ROM images. Otherwise a warning that names the
// files it lacks ("`what` is for the IBM 3487, which needs ROM images that
// are not in the ROM folder: ..."; what = "": "The IBM 3487 needs ...") and
// offers to import them in the ROM manager; true when they are there
// afterwards.
bool ensure_model_roms(HWND owner, const UserDirs& dirs, const Model& model, const std::string& what);

// Edit > Settings (application-wide: speed, default TLS minimum, files).
bool settings_dialog(HWND owner, const UserDirs& dirs, AppSettings& s);

// The CRT type as the profile editor shows it ("Paper white", "Colour").
const char* subtype_label(DisplaySubtype d);

enum class ProfileEditMode {
    Connect,   // OK button says "Connect"
    Save,      // OK button says "Save"
    Apply,     // OK button says "OK" (Edit > Connection Settings)
};
// The terminal type list shows every model that can be emulated; those
// whose ROM images are not in the ROM folder are greyed and say so, and
// choosing one offers to import them (ensure_model_roms).
bool profile_editor_dialog(HWND owner, Profile& p, ProfileEditMode mode, const AppSettings& s,
                           const UserDirs& dirs, const std::string& title);

// TLS Settings (from the profile editor): verify mode, fingerprint with a
// Fetch button that reads the certificate of the first TLS session off the
// UI thread,
// and the optional minimum version.
bool tls_settings_dialog(HWND owner, Profile& p, const AppSettings& s);

// File > Connect to Profile: the profiles in the user folder with New,
// Edit, Rename, Copy and Delete. Returns the profile to connect, or "" when
// closed without connecting.
std::string profile_manager_dialog(HWND owner, const UserDirs& dirs, const AppSettings& s);

// A name prompt (new profile, rename, Save As); `name` in and out.
bool name_dialog(HWND owner, const std::wstring& title, const std::wstring& prompt, std::string& name);

// The TLS trust prompt: certificate details and Stop / Connect / Trust
// Permanently.
tn5250::TlsTrustDecision trust_dialog(HWND owner, const tn5250::TlsTrustRequest& req, const std::string& profile);

// The prompt for a certificate that is expired, not yet valid or revoked
// (TlsTrustProblem::Invalid): every error, the certificate, and Cancel or
// Connect Anyway behind an "I understand the risk" box. True: connect, this
// once. There is no way to remember the answer.
bool certificate_invalid_dialog(HWND owner, const tn5250::TlsTrustRequest& req);

// Edit > Keymap: the global keymap's entries; add, edit (with key
// capture), remove, reset to the PCOMM default.
bool keymap_dialog(HWND owner, KeymapFile& file);

void about_dialog(HWND owner, const UserDirs& dirs);

// Per-user file type registration for .em34xx (HKCU\Software\Classes).
bool register_file_association(std::string& error);

} // namespace win32
} // namespace em34xx
