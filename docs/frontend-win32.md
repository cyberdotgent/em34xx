# em34xxw: the Win32 front end

`em34xxw.exe` is the end-user build for Windows 7 and later (x86 and x64):
a GUI-subsystem program with menus, a toolbar and a status bar, connection
profiles, a ROM manager and a keymap editor. It links `em34xx_runtime` and
`em34xx_frontend_win32` only; the monitor, the GDB stub and the other
developer tools are not part of it (use `em34xx.exe` / `em34xx-headless.exe`
for those).

Sources: `src/main_win32.cpp`, `src/frontend/win32/`:

| File | What |
| --- | --- |
| `win32_frontend.*` | launch (user folder, settings, first-start ROM check, command line), the main window, picture, keyboard, profiles and connections, the TLS trust queue |
| `win32_dialogs.*` | ROM images, Settings, profile manager and editor, TLS Settings, the trust prompt, Keymap, About, file association |
| `win32_util.*` | UTF-8/UTF-16, DPI helpers (newer APIs loaded at run time), scan code -> HID table, message boxes, the open-file dialog |
| `res/notices.txt` | third-party licence notices, embedded as RCDATA and shown in Help > About (no separate notices file ships) |
| `version.h.in` | generated `em34xxw_version.h`: version (from `project(VERSION)`) and project page (`EM34XX_HOMEPAGE_URL`, empty = "to be announced") for VERSIONINFO and About |
| `em34xxw.rc`, `resource.h` | menu, accelerators, dialog templates (`DIALOGEX`), string table, icons, toolbar bitmaps, version information, manifest; compiled by windres |
| `em34xxw.manifest` | Common Controls 6 (visual styles), DPI awareness, `supportedOS`, `asInvoker` |
| `res/` | icons and toolbar strips, drawn by `tools/gen_win32_assets.py` (original artwork) |

Unicode (W) APIs only (`UNICODE`/`_UNICODE`); nothing newer than Windows 7
is linked: `GetDpiForWindow`, `AdjustWindowRectExForDpi`,
`SystemParametersInfoForDpi` and `EnableNonClientDpiScaling` are looked up
with `GetProcAddress` and fall back to the system DPI.

## Start-up

1. The user folder (below) is created if needed and `settings.toml` read.
2. The command line: `em34xxw [PROFILE.em34xx]`. A profile path opens and
   connects that profile once the window is up, so a file association can
   start it. Anything else is reported in a message box.
3. The model: the settings' `model` when it is runnable
   (`runnable_models()`: buildable, a complete ROM dump known), else the
   first runnable one (today the IBM 3477).
4. **First start**: when the model's ROM images are missing or damaged, the
   ROM dialog opens before the main window; closing it without usable ROMs
   ends the program.
5. The global keymap (`keymap.toml`) is checked; a broken file is reported
   and the built-in PCOMM map is used.
6. The `Application` starts with a command line built from all this
   (`--model`, `--rom-dir`, `--display`, `--speed`, `--keymap`, `--nvram`,
   `--log info --log-file <folder>\em34xxw.log`, `--no-crt`). Errors from it
   go to a message box (`Application::last_error()`).

## The user folder

`%APPDATA%\em34xx` (`SHGetFolderPathW(CSIDL_APPDATA)`, available on
Windows 7), or the folder named by the environment variable
`EM34XX_USER_DIR` (portable use and tests). The layout is
`config/user_dirs.hpp`, platform independent and tested with an injected
root:

```
%APPDATA%\em34xx\
  settings.toml        application settings
  keymap.toml          the global keymap (only once edited; else PCOMM)
  roms\                ROM images (the emulator's rom-dir)
  profiles\*.em34xx    connection profiles
  3477.nvram           battery-backed RAM of the terminal (its setup)
  em34xxw.log          the log of the last run
```

The emulator core opens files by narrow name; for a user folder with
characters outside ASCII, the paths handed to it are converted to their
8.3 short form (or the ANSI code page), and ROM files are opened by UTF-8
name. The front end's own files use wide paths.

## settings.toml

Global, not per profile (`config/settings.hpp`):

```toml
model = "3477"
[display]
subtype = "green"        # CRT type: green, amber, paperwhite, color (next start)
crt = true               # scan-line rendering (immediately)
[emulator]
speed = 100              # percent; 0 = unthrottled (immediately)
[tls]
min_version = "1.2"      # default minimum for profiles that set none
[window]
zoom = 1.0               # View > Zoom: 1.0, 1.5 or 2.0
x = 0                    # last placement (restored at start)
y = 0
width = 1099
height = 925
maximized = false
```

Unknown keys are ignored and missing ones take defaults. Files are written
to a temporary name and renamed over the old one.

## Profiles (`.em34xx`)

TOML, read and written by `config/profile.*`. A profile never contains a
password.

```toml
format = 1
model = "3477"                  # terminal emulation type

[tls]
enabled = true
verify = "fingerprint"          # "system" (verify normally), "none" (accept
                                # all certificates) or "fingerprint"
cert_sha256 = "93:71:D2:...:31:95"   # with "fingerprint"
min_version = "1.2"             # optional; absent = the settings' default

[[session]]                     # session 1: Twinax station 0 (required)
host = "pub400.com"
port = 992
device_name = "QPADEV0001"      # optional DEVNAME

[[session]]                     # session 2: station 1 (optional)
host = "other.example"
port = 992
cert_sha256 = "AB:CD:..."       # optional: this session's trusted
                                # certificate when it is another system
```

A profile becomes the bridge configuration through
`profile_connect_config()`: the TN5250 bridge on the line, one display
station per used session, `[tn5250]` from session 1 and the TLS settings,
and for session 2 a station with its own `host`/`port` when it differs
(new `StationConfig` overrides, also accepted in `[[twinax.station]]` of
the TOML configuration) and its own `tls_cert_sha256`.

Names are file names in `profiles\` (characters Windows forbids and
reserved device names are refused). A newer `format` is refused with a
message.

## Menus and toolbar

| Menu | Item | Toolbar | What |
| --- | --- | --- | --- |
| File | Connect to Profile... (Ctrl+Shift+O) | yes | the profile manager: the profiles in `profiles\` with Connect, New..., Edit..., Rename..., Copy, Delete |
| File | Connect... (Ctrl+Shift+N) | yes | the profile editor for a new, unsaved profile; connecting makes the title "Untitled Profile*" |
| File | Edit Profile... | | the current profile in the editor; OK reconnects with it (marked `*` when changed) |
| File | Save (Ctrl+Shift+S) | yes | save the current profile; on an untitled one, Save As |
| File | Save As... | | save under a new name in `profiles\` |
| File | Disconnect (Ctrl+Shift+D) | yes | take the TN5250 bridge off the line |
| File | Exit | | asks to save an unsaved or changed profile |
| Edit | Keymap... | yes | the keymap editor |
| Edit | Settings... | yes | CRT type, CRT rendering, speed, default TLS minimum, ROM images, file association |
| View | Zoom > 1x / 1.5x / 2x | | radio items; the picture area becomes the presented frame times the zoom times the DPI scale (shrunk to the work area if it does not fit); manual resizing stays possible and choosing a zoom snaps back. Saved in settings.toml |
| Terminal | Attn, SysRq, Reset | yes (text buttons) | press the terminal's own key through the keyboard model (`Machine::terminal_key()`, set-3 position with the Shift/Alt its legend needs), so the firmware and the controller see the real key |
| Terminal | Power Cycle | | `Application::power_on()` |
| Help | About... | | version, user folder, TLS backend, project page, third-party notices |

The window title is `<profile>[*] - IBM 3477 - em34xx`; `*` marks a
profile that differs from its file (or was never saved). The status bar
shows session 1, session 2 (state and host:port, with the last error), the
TLS state (off, not connected, verified, trusted fingerprint, not
verified, accepted once) and the profile.

Accelerators use Ctrl+Shift only; every other key belongs to the terminal.

## The profile editor and TLS Settings

The editor has the terminal type (a combo box filled from
`runnable_models()`; only "IBM 3477" today), two sessions (host, port,
optional device name; session 2 may stay empty), **Enable TLS** (it also
swaps the conventional ports 23 and 992) and **TLS Settings...**:

- *Verify normally*: the Windows certificate store and the host name.
- *Accept all certificates* (insecure).
- *Accept only the certificate with this SHA-256 hash*, with **Fetch**:
  it connects to session 1's host and port on a worker thread
  (`tls_fetch_certificate()`), shows subject, issuer, validity, the
  fingerprint and whether it verifies, and fills in the hash. The dialog
  can close while a fetch runs; the result is then dropped.
- *Minimum version*: Default (the settings' value) or SSL 3.0 ... TLS 1.3
  (SChannel offers up to TLS 1.2).

## The TLS trust prompt

When TLS is on and a certificate is refused (not trusted, wrong name,
expired, or not the pinned one), the session's I/O thread asks the front
end through the trust hook (`docs/tn5250.md`, "Asking the user"). The
request is queued and the thread waits; the window shows **Untrusted
Certificate** with the reason and the certificate (subject, issuer, valid
from/until, SHA-256), and three answers:

- **Stop**: the connection is refused and that station's session is held
  (no reconnect loop into the same question).
- **Connect**: this connection only; the profile is unchanged. Reconnects
  of the same connection do not ask again; a new Connect does.
- **Trust Permanently**: the fingerprint goes into the profile
  (`profile_trust_certificate()`: fingerprint mode for session 1, or a
  session-2 `cert_sha256` when session 2 is another host). A saved profile
  is saved at once; an untitled one keeps it in memory and is marked `*`.

Disconnect, a new connection and Exit answer any waiting request with Stop
first, so no session thread stays blocked when it is joined.

## Picture

The emulator's display callback posts `WM_APP` to the window (one message
in flight); the raster is fetched under the machine lock at most every
15 ms (a one-shot timer covers the rest), turned into the tube image by the
shared `CrtRenderer` at the picture area's pixel size, and only the rows
that changed are invalidated. `WM_PAINT` copies it with `SetDIBitsToDevice`
(top-down 32-bit DIB) and paints the borders black. The toolbar's
transparent background is painted by the main window.

## Keyboard

`WM_KEYDOWN`/`WM_KEYUP`/`WM_SYSKEYDOWN`/`WM_SYSKEYUP` become USB HID usages
from the scan code (lParam bits 16-23) and the extended bit 24
(`hid_from_key_message()`; Pause and Num Lock, which share scan code 45,
by virtual key). Auto-repeats (bit 30) are dropped: the terminal does its
own typematic. AltGr's fake Left Ctrl (same time stamp as the Right Alt)
is dropped. Keys go to the main window without `TranslateMessage`, so Alt
and F10 reach the terminal and do not open the menu; the menu stays
available by mouse, and Alt+F4 still closes the window. Losing the focus
releases every key the terminal thinks is held.

## Keymap editor

Edit > Keymap shows the global keymap's entries (PC key, modifiers,
terminal key, keyboard type, options) from `keymap.toml`, or the PCOMM
default (`pcomm_keymap_file()`, the same rules as
`keymaps/pcomm-like.toml`). Add/Edit open the mapping dialog: click the
capture box and press the key with its modifiers, or pick the key from the
list; the terminal key is a name from `terminal_function_names()` (or
`0x5A`, `none`, `positional`); keyboard type and tap are optional. Remove
and **Reset to Default (PCOMM)**. OK writes `keymap.toml`
(`keymap_file_to_toml()`) and applies it at once (`Machine::set_keymap()`).

## DPI

The manifest declares `dpiAware` `true/pm` and `dpiAwareness`
`PerMonitorV2, PerMonitor`: Windows 7/8 run system aware, 8.1 per monitor,
Windows 10 1703+ per monitor v2 (dialogs and the non-client area scale
automatically). The toolbar strip is picked by DPI (16 px below 120 dpi,
24 px below 168, else 32 px), the bars get a font for the DPI, zoom sizes
are multiplied by DPI/96, and `WM_DPICHANGED` applies the suggested
rectangle and rebuilds the toolbar images.

## File association

Settings > "Open .em34xx files with em34xxw..." asks first, then writes
per-user keys (never automatically, never for all users):

```
HKCU\Software\Classes\.em34xx                         = em34xx.Profile
HKCU\Software\Classes\em34xx.Profile                  = em34xx connection profile
HKCU\Software\Classes\em34xx.Profile\DefaultIcon      = "<exe>",-2
HKCU\Software\Classes\em34xx.Profile\shell\open\command = "<exe>" "%1"
```

## Testing (wine)

Tests never touch the user's desktop or `~/.wine`: a private `Xvfb`, a
`WINEPREFIX` in a scratch folder, `wine explorer /desktop=...` for window
decorations. In that virtual desktop wine stacks popups below the main
window in X, so screenshots capture each em34xxw window (`xwd -id`) and
composite them. Observed (November 2026, wine 9.0, x86 and x64 builds):

- First start without ROMs: the ROM dialog; importing renamed dumps stores
  them under the expected names (recognised by SHA-256); the 3477 then
  boots (Offline Setup with a blank NVRAM, the on-line screen with a
  configured one).
- Ad-hoc Connect to pub400.com:23: "Untitled Profile*", sign-on screen,
  typing reaches the field; Save As writes `profiles\PUB400.em34xx`; the
  profile manager's Copy, Rename and Delete; a profile path on the command
  line connects at start; closing asks to save an untitled profile.
- TLS: pub400.com:992 verifies (no prompt). A local self-signed
  `openssl s_server`: Fetch shows the certificate and fills the hash; the
  trust prompt shows the SChannel reason (`CERT_E_UNTRUSTEDROOT`); Trust
  Permanently writes fingerprint mode; the pinned profile then connects
  without asking; Stop holds the session.
- Keymap: key capture (Ctrl+F5), saved `keymap.toml`.
- View > Zoom at 96 dpi: picture 1091x818, 1637x1227, 2182x1636; at
  144 dpi, 1x gives 1637x1227 and the 24 px toolbar; dialogs scale.
- The file association, in the wine prefix's registry only.

Not tested: real Windows 7/10/11, `WM_DPICHANGED` between monitors, a
non-ASCII user folder.

## Known gaps

- Sound: no `AudioSink` yet (the alarm is silent).
- The CRT type (display subtype) and the model apply at the next start;
  a profile for another model asks for a restart.
- Printer sessions are not offered (the bridge has only a placeholder).
- No per-profile keymap (by design: one global keymap).

## Version and notices

The version comes from `project(em34xx VERSION ...)` in CMakeLists.txt and
appears in the VERSIONINFO resource and Help > About (major.minor). Set the
project page with `-DEM34XX_HOMEPAGE_URL=https://...`; About then shows it as
a link, which opens in the default browser.

`res/notices.txt` holds the licence notices of the third-party code linked
into em34xxw (toml++, the mingw-w64 runtime, winpthreads, the GCC runtime).
No separate notices file is shipped; the About dialog is where they are
displayed. Update the file when a dependency is added to the Win32 build
(`x86_64-w64-mingw32-objdump -p` lists the DLLs; static libraries come from
`vcpkg_installed/<triplet>/share/<port>/copyright`).
