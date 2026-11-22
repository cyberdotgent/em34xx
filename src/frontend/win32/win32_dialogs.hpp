// SPDX-License-Identifier: GPL-3.0-or-later
// em34xxw dialogs (templates in em34xxw.rc): ROM images, settings, profile
// manager and editor, TLS settings, the TLS trust prompt, the keymap
// editor and About. All modal; each returns once the user closes it.
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
// summary says whether it is ready). True when `required` is ready at close
// (always true without one).
bool rom_manager_dialog(HWND owner, const UserDirs& dirs, const Model* required);

// Edit > Settings (application-wide: speed, default TLS minimum, files).
bool settings_dialog(HWND owner, const UserDirs& dirs, AppSettings& s);

// The CRT type as the profile editor shows it ("Paper white", "Colour").
const char* subtype_label(DisplaySubtype d);

enum class ProfileEditMode {
    Connect,   // OK button says "Connect"
    Save,      // OK button says "Save"
    Apply,     // OK button says "OK" (Edit > Connection Settings)
};
bool profile_editor_dialog(HWND owner, Profile& p, ProfileEditMode mode, const AppSettings& s,
                           const std::string& title);

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

// Edit > Keymap: the global keymap's entries; add, edit (with key
// capture), remove, reset to the PCOMM default.
bool keymap_dialog(HWND owner, KeymapFile& file);

void about_dialog(HWND owner, const UserDirs& dirs);

// Per-user file type registration for .em34xx (HKCU\Software\Classes).
bool register_file_association(std::string& error);

} // namespace win32
} // namespace em34xx
