// SPDX-License-Identifier: GPL-3.0-or-later
// em34xx.app dialogs, built in code: ROM images, the Settings window,
// profile manager and editor, TLS settings, the TLS trust prompt, the
// invalid-certificate prompt, the keymap editor and About. Except Settings
// (a window of its own whose changes apply at once, as Mac settings
// windows do), each is a sheet on `parent` (app-modal without one) and
// returns once the user closes it. docs/frontend-cocoa.md.
#pragma once

#import <Cocoa/Cocoa.h>

#include "config/profile.hpp"
#include "config/settings.hpp"
#include "config/user_dirs.hpp"
#include "input/host_keymap.hpp"
#include "tn5250/tls_trust.hpp"

#include <functional>
#include <string>

namespace em34xx {

class Model;

namespace cocoa {

// ROM images: what each model needs and its state; import (also by
// dropping files on the list), remove, re-check. `required`: the model
// about to run (its rows come first, the summary says whether it is ready).
// True when `required` is ready at close (always true without one).
bool rom_manager_dialog(NSWindow* parent, const UserDirs& dirs, const Model* required);

// em34xx > Settings: startup profile, speed, default TLS minimum, ROM
// images, the user folder. Changes apply at once: `settings` is updated and
// `changed(before)` called (the caller applies and saves them).
void show_settings_window(const UserDirs& dirs, AppSettings& settings,
                          std::function<void(const AppSettings& before)> changed);
// Refill the startup profile list (profiles were added or renamed).
void refresh_settings_window();

// The CRT type as the profile editor shows it ("Paper white", "Colour").
const char* subtype_label(DisplaySubtype d);

enum class ProfileEditMode {
    Connect,   // default button says "Connect"
    Save,      // "Save"
    Apply,     // "OK" (File > Connection Settings)
};

bool profile_editor_dialog(NSWindow* parent, Profile& p, ProfileEditMode mode, const AppSettings& s,
                           const std::string& title);

// TLS Settings (from the profile editor): verify mode, fingerprint with a
// Fetch button that reads the certificate of the first TLS session off the
// main thread, and the optional minimum version.
bool tls_settings_dialog(NSWindow* parent, Profile& p, const AppSettings& s);

// File > Connect to Profile: the profiles in the user folder with New,
// Edit, Rename, Duplicate and Move to Trash. Returns the profile to
// connect, or "" when closed without connecting.
std::string profile_manager_dialog(NSWindow* parent, const UserDirs& dirs, const AppSettings& s);

// A name prompt (new profile, rename, Save As); `name` in and out. The
// name is checked with UserDirs::valid_profile_name().
bool name_dialog(NSWindow* parent, const std::string& title, const std::string& prompt, const std::string& ok,
                 std::string& name);

// The TLS trust prompt: certificate details and Stop / Connect / Trust
// Permanently. Stop is the default.
tn5250::TlsTrustDecision trust_dialog(NSWindow* parent, const tn5250::TlsTrustRequest& req,
                                      const std::string& profile);

// The prompt for a certificate that is expired, not yet valid or revoked
// (TlsTrustProblem::Invalid): every error, the certificate, and Cancel or
// Connect Anyway behind an "I understand the risk" box. True: connect, this
// once. There is no way to remember the answer.
bool certificate_invalid_dialog(NSWindow* parent, const tn5250::TlsTrustRequest& req);

// Edit > Keymap: the global keymap's entries; add, edit (with key
// capture), remove, reset to the default (`default_file`: the Mac map).
bool keymap_dialog(NSWindow* parent, KeymapFile& file, const KeymapFile& default_file);

// em34xx > About em34xx: the standard About panel with the version, the
// user folder, the TLS backend, the project page and the third-party
// notices (notices.txt in the bundle's resources).
void show_about_panel(const UserDirs& dirs);

} // namespace cocoa
} // namespace em34xx
