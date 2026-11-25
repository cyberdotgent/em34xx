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
| `win32_dialogs.*` | ROM images, Settings, profile manager and editor, TLS Settings, the trust prompt, the invalid-certificate prompt, Keymap, About, file association |
| `win32_util.*` | UTF-8/UTF-16, DPI helpers (newer APIs loaded at run time), scan code -> HID table, message boxes, the open-file dialog, the exception guard for window and dialog procedures |
| `res/notices.txt` | third-party licence notices, embedded as RCDATA and shown in Help > About (no separate notices file ships) |
| `version.h.in` | generated `em34xxw_version.h`: version (from `project(VERSION)`) and project page (`EM34XX_HOMEPAGE_URL`, empty = "to be announced") for VERSIONINFO and About |
| `em34xxw.rc`, `resource.h` | menu, accelerators, dialog templates (`DIALOGEX`), string table, icons, toolbar bitmaps, version information, manifest; compiled by windres |
| `em34xxw.manifest` | Common Controls 6 (visual styles), DPI awareness, `supportedOS`, `asInvoker` |
| `res/` | icons and toolbar strips, drawn by `tools/gen_win32_assets.py` (original artwork); the application icon is a 3477 behind its 122-key keyboard as Windows 9x pixel art (black outline, short palette, no anti-aliasing), drawn per size (16 to 64 px; from 64 px the keyboard is drawn key by key) after photographs of the terminal, with a 256 px entry (the 64 px drawing four times over) for Explorer's large views |

Unicode (W) APIs only (`UNICODE`/`_UNICODE`); nothing newer than Windows 7
is linked: `GetDpiForWindow`, `AdjustWindowRectExForDpi`,
`SystemParametersInfoForDpi` and `EnableNonClientDpiScaling` are looked up
with `GetProcAddress` and fall back to the system DPI.

`WinMain` first takes the current directory out of the DLL search path
(`SetDllDirectoryW(L"")`), restricts later loads by name to the system
folder where the system can (`SetDefaultDllDirectories`, Windows 7 with
KB2533623 and later; looked up at run time) and ends the process on heap
corruption. No C++ exception crosses a system callback: `WinMain`, the
window procedure and every dialog procedure catch them (`win32::guarded`)
and report them in a message box; the operation is abandoned, the program
keeps running (from `WinMain`: exit code 3).

## Start-up

1. The user folder (below) is created if needed and `settings.toml` read.
2. The command line: `em34xxw [PROFILE.em34xx]`. A profile path opens and
   connects that profile once the window is up, so a file association can
   start it; a file outside the user's `profiles\` folder is confirmed
   first ("Profiles from outside", below). Anything else is reported in a
   message box.
3. The terminal starts as the profile that will be opened describes it
   (the command-line profile, else the startup profile: its model, CRT type
   and scan lines); without one, as a new profile would: the first model
   whose ROM images are in the ROM folder (`choose_start_model()`), green,
   with scan lines. The runnable models (`runnable_models()`: buildable, a
   complete ROM dump known) are the IBM 3477, 3486 and 3487; the last two
   share one image.
4. **ROM images**: one complete set is enough to start. The images of the
   other models may be missing; those models stay in the terminal type
   list, greyed, until their images are imported.
   - A profile for a model without its images: a warning names the profile,
     the model and each missing file (`54g0010.bin - system ROM ...:
     missing`) and offers to import them (the ROM dialog). Declined, the
     program starts as another model that can run, without the profile.
   - **First start** (no model can run): the ROM dialog opens before the
     main window and says that one model's images are needed; closing it
     with none ends the program.
5. The global keymap (`keymap.toml`) is checked; a broken file is reported
   and the built-in PCOMM map is used.
6. The `Application` starts with a command line built from all this
   (`--model`, `--rom-dir`, `--phosphor`, `--speed`, `--keymap`, `--nvram`,
   `--unmapped log`, `--log info --log-file <folder>\em34xxw.log`,
   `--no-crt`). `--unmapped log`: an access the model does not decode is
   logged and the terminal keeps running (the emulator's default, `stop`,
   is for the debugger and would leave the terminal dead). Errors from the
   start go to a message box (`Application::last_error()`).
   The environment variable `EM34XX_LOG` replaces `info` with any `--log`
   spec, e.g. `info,wsc=debug,tn5250=debug,input=debug` to see each key,
   the scan code the controller decodes and each record sent to the host
   (docs/keyboard.md, "Diagnosing input").
7. The main window opens. With a profile on the command line, that
   profile connects (below); otherwise the startup profile does when
   Settings > Startup has "Open session on startup" checked and names a
   profile that still exists (a name only: a `startup.profile` in
   settings.toml that is not a valid profile name, such as a path, is
   ignored); otherwise **Connect to Profile** (the profile
   manager) opens at once, modal over the main window and centred on it.
   Close/Cancel leaves the terminal idle and not connected.

## Connecting: power cycle first

Every way of opening a profile (New Connection..., Connect to Profile, a
profile on the command line) first applies the profile's terminal settings
(CRT type, scan lines; a CRT type the firmware or host can see rebuilds the
machine, which then powers on by itself) and then goes through
`Application::connect_after_power_cycle()`: the TN5250 sessions that run
are closed (any waiting trust request is answered Stop), the terminal is
reset as at power-on and runs its POST, and the bridge is put on the line.
While POST runs the terminal does not answer polls: the controller keeps
the station absent and probes it (`docs/twinax-bridge.md`); once the
terminal answers, it is identified and the session's screen is written, so
the sign-on screen follows POST with no manual step (observed with the
3477 ROMs against pub400.com:23, below).

Reconnecting (the Disconnect toolbar button when off line, or File >
Reconnect) and connection changes made in Edit > Connection Settings plug
the bridge back in without a power cycle: the terminal keeps running, as
when a cable is plugged back in.

**Terminal > Reboot** (and its toolbar button) power-cycles the terminal
(`Application::power_on()`). Its sessions end, as on a real host: the
controller is told, closes them and opens new ones, and the terminal comes
back on line to a new sign-on (docs/twinax-bridge.md, "Session and
terminal lifecycle"). After File > Disconnect, a reboot also dials the host
again. The controller keeps its own clock across the terminal's reset
(`TwinaxEngine` hands the host a time that never goes back).

## Dialogs

Every dialog and message box is centred on its owner (the main window, or
the dialog that opened it), kept inside that monitor's work area: a
thread-local `WH_CBT` hook (`install_dialog_centring()`) moves each
`#32770` window once, when it is first activated. The first-start ROM
dialog has no owner and stays centred on the screen (`DS_CENTER`).

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
  tube.burn            the wear of the tube (docs/burn-in.md)
  em34xxw.log          the log of the last run
```

The emulator core opens files by narrow name; for a user folder with
characters outside ASCII, the paths handed to it are converted to their
8.3 short form (or the ANSI code page), and ROM files are opened by UTF-8
name. The front end's own files use wide paths.

## settings.toml

Application-wide (`config/settings.hpp`); the terminal model and its
display belong to the profile. em34xx 0.1 kept `model` and `[display]`
here; they are now ignored.

```toml
[startup]
open_profile = false     # open `profile` at start (Settings > Startup)
profile = ""
[emulator]
speed = 100              # percent; 0 = unthrottled (immediately)
[tls]
min_version = "1.2"      # default minimum for profiles that set none
[display]
burn_in = "slow"         # View > Burn-in: off, slow, fast or hyper
[window]
zoom = 1.0               # View > Zoom: 1.0, 1.5 or 2.0
x = 0                    # last placement (restored at start)
y = 0
width = 1099
height = 925
maximized = false
```

Speed applies at once; the TLS minimum at the next connect.

Unknown keys are ignored and missing ones take defaults. Files are written
to a temporary name and renamed over the old one.

### Applying a profile's terminal settings

| Setting | Applies |
| --- | --- |
| CRT type (the phosphor of a monochrome unit) | at once, picture only (`Application::set_phosphor()`); no reset, the sessions stay up. Paper-white too: it is a tube, not another unit |
| scan lines | at once |
| terminal type (model or variant, e.g. 3487-HG -> 3487-HC) | the emulator restarts as the other unit (`Application::set_model()`): its ROM images are read, a model gets its own battery-backed RAM file (`<model>.nvram`, shared by its variants; a variant with its own firmware has `<variant>.nvram`, e.g. `3477-FC.nvram`, docs/models.md), the sessions close and new ones announce its terminal type, and the title changes. Opening a profile does it without asking; Connection Settings asks first. Without the model's ROM images the user is told which files are missing and may import them; otherwise nothing changes |

The terminal type list shows every variant of every model (docs/models.md);
the CRT type list shows the phosphors of the selected variant and is
disabled ("Colour") for a colour unit.

## Profiles (`.em34xx`)

TOML, read and written by `config/profile.*`. A profile never contains a
password.

```toml
format = 3
model = "3477-FG"               # terminal type: a model's variant (docs/models.md)

[display]
phosphor = "green"              # CRT type of a monochrome variant
crt = true                      # scan-line rendering

[tls]                           # for the sessions with tls = true
verify = "fingerprint"          # "system" (verify normally), "none" (accept
                                # all certificates) or "fingerprint"
cert_sha256 = "93:71:D2:...:31:95"   # with "fingerprint"
min_version = "1.2"             # optional; absent = the settings' default

[[session]]                     # session 1: Twinax station 0 (required)
host = "pub400.com"
port = 992
tls = true                      # each session with or without TLS
device_name = "QPADEV0001"      # optional DEVNAME

[[session]]                     # session 2: station 1 (optional)
host = "other.example"
port = 992
tls = true
cert_sha256 = "AB:CD:..."       # optional: this session's trusted
                                # certificate when it is another system
```

A profile becomes the bridge configuration through
`profile_connect_config()`: the TN5250 bridge on the line, one display
station per used session, `[tn5250]` from session 1 and the TLS settings,
and for session 2 a station with its own `host`/`port` when it differs,
its own `tls` on or off and its own `tls_cert_sha256` (`StationConfig`
overrides, also accepted in `[[twinax.station]]` of the TOML
configuration; `tls = false` there also drops `[tn5250]`'s pinned
certificate and CA file for that station). With session 1 plain and
session 2 on TLS, a fingerprint pin goes to session 2's station.

Format 1 (em34xx 0.1) had one `tls.enabled` for both sessions and no
`[display]`; such files still load (each session takes `tls.enabled`, the
display its defaults) and are saved as format 2. em34xx 0.1 refuses
format 2 ("written by a newer em34xx").

Names are file names in `profiles\` (characters Windows forbids and
reserved device names are refused). A newer `format` is refused with a
message.

### Profiles from outside

A profile carries its own TLS settings, so a file from someone else can
turn verification off or pin its sender's certificate. When the profile
named on the command line (a double-clicked `.em34xx` file) is not in the
user's own `profiles\` folder (`win32::file_in_folder()`: the same
directory on disk, whatever the spelling of the path), em34xxw asks before
it connects: the file, each session's host:port and how its certificate is
checked ("TLS off", "TLS, verified", "TLS, only the certificate with
SHA-256 ...", "TLS, certificate NOT VERIFIED"), default button No. No
leaves the terminal idle and not connected. The profile manager and the
startup profile only open files in `profiles\` and do not ask.

## Menus and toolbar

| Menu | Item | Toolbar | What |
| --- | --- | --- | --- |
| File | New Connection... (Ctrl+Shift+N) | yes | the profile editor for a new, unsaved profile; connecting makes the title "Untitled Profile*" |
| File | Connect to Profile... (Ctrl+Shift+O) | yes | the profile manager: the profiles in `profiles\` with Connect, New..., Edit..., Rename..., Copy, Delete; also shown at start without a command-line profile |
| File | Save (Ctrl+Shift+S) | yes | save the current profile; on an untitled one, Save As |
| File | Save As... | | save under a new name in `profiles\` |
| File | Disconnect / Reconnect (Ctrl+Shift+D) | yes (a toggle) | on line: take the TN5250 bridge off the line; off line: reconnect the current profile without a power cycle. The button's picture and tooltip follow the state |
| File | Exit | | asks to save an unsaved or changed profile |
| Edit | Connection Settings... (Ctrl+Shift+E) | | the running profile in the editor (OK, Cancel). A change marks the profile modified (`*`; saved with Save, asked about on exit or when another profile is opened) and applies at once: the CRT type and scan lines as in "Applying a profile's terminal settings", connection changes (hosts, ports, devices, TLS) by reconnecting with the terminal running. Without a profile it is New Connection |
| Edit | Keymap... | yes | the keymap editor |
| Edit | Settings... | yes | startup profile, speed, default TLS minimum, screen burn-in, ROM images, file association |
| View | Zoom > 1x / 1.5x / 2x | | radio items; the picture area becomes the presented frame times the zoom times the DPI scale (shrunk to the work area if it does not fit); manual resizing stays possible and choosing a zoom snaps back. Saved in settings.toml |
| View | Burn-in > Off / Slow / Fast / Hyper, Replace CRT... | | screen burn-in (docs/burn-in.md): radio items for the speed, saved in settings.toml; Replace CRT asks, then starts a new tube |
| Terminal | Attn, SysRq, Reset, Setup | yes (text buttons) | press the terminal's own key through the keyboard model (`Machine::terminal_key()`, set-3 position with the Shift/Alt its legend needs), so the firmware and the controller see the real key |
| Terminal | Reboot (Power Cycle) | yes | switch the terminal off and on; its sessions end and it comes back to a new sign-on (see "Connecting") |
| Help | About... | | version, user folder, TLS backend, project page, third-party notices |

The window title is `<profile>[*] - IBM 3477 - em34xx`; `*` marks a
profile that differs from its file (or was never saved).

The status bar shows `Profile: <name>[*]` on the left and, on the right,
per session an orb and a lock (glyphs from `tools/gen_win32_assets.py`,
`res/status*.bmp`, sized for the DPI):

| Orb | Session |
| --- | --- |
| green | connected |
| orange | connecting or negotiating, or waiting on the certificate prompt |
| red | disconnected, closed or failed (the bridge retries unless the user stopped it) |
| grey | not configured in the profile |

The lock is closed for a session with TLS, a dimmed closed lock for TLS
with "Accept all certificates" (encrypted, the server not verified) and
open for plain telnet (dimmed for a session that is not configured).
Hovering an orb or lock
shows the host:port, the state with the last error, and the TLS state
(verified, trusted certificate, not verified, accepted once, or
"certificate NOT VALID (accepted for this connection)" for a session the
user let through the invalid-certificate prompt).

Accelerators use Ctrl+Shift only; every other key belongs to the terminal.

## The profile editor and TLS Settings

The editor has the terminal type (a combo box filled from
`runnable_models()`; a model whose ROM images are not in the ROM folder is
drawn grey and reads "IBM 3477 (ROM images needed)", and choosing it names
the missing files and offers to import them, going back to the previous
choice when they are still missing), two sessions (host, port,
optional device name, **Use TLS** for each session (it also swaps that
session's conventional port, 23 and 992); session 2 may stay empty), the
CRT type and scan lines, and **TLS Settings...** for the sessions that use
TLS:

- *Verify normally*: the Windows certificate store and the host name.
- *Accept all certificates* (insecure).
- *Accept only the certificate with this SHA-256 hash*, with **Fetch**:
  it connects to the first TLS session's host and port on a worker thread
  (`tls_fetch_certificate()`), shows subject, issuer, validity, the
  fingerprint, whether it verifies, a `NOT VALID:` line per validity error
  (expired, not yet valid, revoked) and the revocation status, and fills in
  the hash. A hash does not settle a validity error: connecting to such a
  certificate still asks every time (below). The dialog
  can close while a fetch runs; the result is then dropped.
- *Minimum version*: Default (the settings' value) or SSL 3.0 ... TLS 1.3
  (SChannel offers up to TLS 1.2).

## The TLS trust prompt

When TLS is on and a certificate is refused (not trusted, wrong name,
or not the pinned one), the session's I/O thread asks the front
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

The prompt never opens over another dialog or message box: while one is up
the request stays queued (the session's orb stays orange) and the prompt
follows once it has closed.

Disconnect, a new connection and Exit answer any waiting request with Stop
first, and while the old sessions are being closed a new request is
refused at once (`TrustQueue::begin_drain()`), so no session thread is
blocked on a prompt when it is joined.

## The invalid-certificate prompt

A certificate that is expired, not yet valid or revoked
(`TlsTrustProblem::Invalid`; `docs/tn5250.md`, "Validity and revocation")
gets a different dialog, **Certificate Not Valid** (`IDD_CERT_INVALID`,
`certificate_invalid_dialog()`), instead of the trust prompt, whether or
not the certificate is trusted or pinned:

- a warning icon and the headline: the certificate of host:port (session
  n) is not valid and the connection may not be secure;
- *What is wrong with the certificate*: every error, one per line, as the
  TLS backend worded it (with Windows' code), followed by the trust or pin
  failure when there is one as well;
- *The certificate the server sent*: subject, issuer, valid from/until,
  SHA-256, verification, validity and revocation lines;
- "This cannot be remembered: you are asked again for every connection.";
- an unticked **I understand the risk and want to connect to this server
  anyway**, which enables **Connect Anyway**; **Cancel** is the default
  button. Esc and closing the dialog are Cancel.

There is no "trust permanently" here, and nothing is written to the
profile. Every connection attempt of every session asks again, the
bridge's automatic reconnects included (`TlsTrustBroker` does not remember
these answers). Cancel holds the session, like Stop. The prompt goes
through the same queue as the trust prompt, so the deferral behind another
dialog, the orange orb ("the certificate is not valid: waiting for your
answer"), the ten-minute limit and the drain on disconnect apply
unchanged. A session connected this way says so in its tooltip until the
next Connect; a certificate renewed during an automatic reconnect does not
clear that text (known gap).

## End of the Windows session

`WM_QUERYENDSESSION` is answered TRUE; on `WM_ENDSESSION` the terminal's
battery-backed RAM and setup EEPROM are written at once
(`Application::flush_persistent_state()`) with the tube's wear
(docs/burn-in.md), the window placement and the settings, since
the process may be ended without a clean exit.

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

- Models and ROMs (November 2026, wine 9.0, x86): with only `54g0010.bin`
  the program starts as an IBM 3486 and the editor greys "IBM 3477 (ROM
  images needed)"; choosing it names both 3477 files and, declined, goes
  back; with no ROMs the ROM dialog asks for one model's images; a 3487
  profile on the command line without its image is named with the file;
  connecting a 3487 profile from a running 3477 restarts as the 3487
  (terminal type `IBM-5251-11`).
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
- Start-up (x64, wine 9.0, Xvfb 1280x1000, main window at 180,60): without
  a profile, Connect to Profile opens centred on the main window (dialog
  centre = window centre) and the main window is disabled (its menu does
  not open); Connect on PUB400 (pub400.com:23) shows the POST test
  pattern, then the sign-on screen. A profile on the command line skips
  the dialog and connects (POST, then sign-on).
- Settings, green -> amber: the picture turns amber at once, the same
  session (same QPADEVxxxx) stays. Amber -> colour: the question (centred
  on the main window); Yes rebuilds (POST in colour), reconnects as
  `IBM-3477-FC` and shows the sign-on screen in colour.

Not tested: the invalid-certificate prompt (built for x86 and x64, not yet
run; its decision path is covered by `tests/test_tn5250_tls.cpp` and the
SChannel classification by `tn5250-probe.exe` under wine), real Windows 7/10/11, `WM_DPICHANGED` between monitors, a
non-ASCII user folder.

## Known gaps

- Sound: no `AudioSink` yet (the alarm is silent).
- A colour 3486/3487 has its monitor alignment marked done by the model
  (a temporary stand-in, docs/bringup-3486.md); a new unit runs its self
  test twice at the first start. The setup EEPROM is kept per model
  (`<model>.eeprom` next to `<model>.nvram`).
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
