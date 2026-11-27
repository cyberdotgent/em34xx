// SPDX-License-Identifier: GPL-3.0-or-later
// em34xx-qt dialogs: ROM images, settings, profile manager and editor, TLS
// settings, the TLS trust prompt, the invalid-certificate prompt, the keymap
// editor and About. All modal; each returns once the user closes it. They
// follow em34xxw's (frontend/win32/win32_dialogs.hpp, docs/frontend-win32.md)
// one for one.
#pragma once

#include "config/profile.hpp"
#include "config/settings.hpp"
#include "config/user_dirs.hpp"
#include "input/host_keymap.hpp"
#include "tn5250/tls_trust.hpp"

#include <string>

class QWidget;

namespace em34xx {

class Model;

namespace qt {

// ROM images: what each model needs and its state; import, remove,
// re-check. `required`: the model about to run (its rows come first).
// `need_any`: no model can run yet and one must (first start). True when
// `required` is ready at close; without one, when some model is ready
// (need_any) or always. `required_variant`: the variant of `required` about
// to run (null: any).
bool rom_manager_dialog(QWidget* owner, const UserDirs& dirs, const Model* required, bool need_any = false,
                        const ModelVariant* required_variant = nullptr);

// True when `model` has its ROM images. Otherwise a warning that names the
// files it lacks and offers to import them in the ROM manager; true when
// they are there afterwards.
bool ensure_model_roms(QWidget* owner, const UserDirs& dirs, const Model& model, const std::string& what,
                       const ModelVariant* variant = nullptr);

// Edit > Settings (application-wide: startup profile, speed, default TLS
// minimum, burn-in, ROM images, file association).
bool settings_dialog(QWidget* owner, const UserDirs& dirs, AppSettings& s);

enum class ProfileEditMode {
    Connect,   // the OK button says "Connect"
    Save,      // "Save"
    Apply,     // "OK" (Edit > Connection Settings)
};
bool profile_editor_dialog(QWidget* owner, Profile& p, ProfileEditMode mode, const AppSettings& s,
                           const UserDirs& dirs, const std::string& title);

// TLS Settings (from the profile editor): verify mode, fingerprint with a
// Fetch button that reads the first TLS session's certificate off the UI
// thread, and the optional minimum version.
bool tls_settings_dialog(QWidget* owner, Profile& p, const AppSettings& s);

// File > Connect to Profile: the profiles in the user folder with New,
// Edit, Rename, Copy and Delete. Returns the profile to connect, or "".
std::string profile_manager_dialog(QWidget* owner, const UserDirs& dirs, const AppSettings& s);

// A name prompt (new profile, rename, Save As); `name` in and out.
bool name_dialog(QWidget* owner, const std::string& title, const std::string& prompt, std::string& name);

// The TLS trust prompt: certificate details and Stop / Connect / Trust
// Permanently.
tn5250::TlsTrustDecision trust_dialog(QWidget* owner, const tn5250::TlsTrustRequest& req,
                                      const std::string& profile);

// A certificate that is expired, not yet valid or revoked: every error, the
// certificate, Cancel or Connect Anyway behind "I understand the risk".
// True: connect, this once.
bool certificate_invalid_dialog(QWidget* owner, const tn5250::TlsTrustRequest& req);

// Edit > Keymap: the global keymap's entries; add, edit (with key
// capture), remove, reset to the PCOMM default, the tn5250 layout.
bool keymap_dialog(QWidget* owner, KeymapFile& file);

void about_dialog(QWidget* owner, const UserDirs& dirs);

// Per-user registration of .em34xx profiles with this executable: a
// shared-mime-info type and a desktop entry under $XDG_DATA_HOME.
bool register_file_association(std::string& error);

} // namespace qt
} // namespace em34xx
