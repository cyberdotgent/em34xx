# em34xx.app: the macOS front end

`em34xx.app` is the end-user build for macOS 11 (Big Sur) and later, one
universal binary for Apple silicon (arm64) and Intel (x86_64). It has the
functions of [em34xxw](frontend-win32.md): connection profiles, the
ROM manager, the keymap editor, the TLS trust prompts, the terminal's own
keys and the session status. It follows macOS conventions where they
differ from Windows (below). It links `em34xx_runtime` and
`em34xx_frontend_cocoa` only; the monitor, the GDB stub and the other
developer tools are not part of it.

Sources: `src/main_cocoa.mm`, `src/frontend/cocoa/` (Objective-C++ with
ARC, AppKit, everything built in code: no nib or storyboard):

| File | What |
| --- | --- |
| `cocoa_frontend.*` | the app delegate (menus, launch, Finder's open requests, the Dock, quitting), one window controller per terminal (picture, keyboard, toolbar, status bar, profiles and connections, the TLS trust queue) |
| `cocoa_dialogs.*` | ROM images, Settings, profile manager and editor, TLS Settings, the trust prompt, the invalid-certificate prompt, Keymap, About |
| `cocoa_util.*` | UTF-8/NSString, sheets run to completion, alerts, Auto Layout helpers, the key code -> HID table, folders, the exception guard |
| `Info.plist.in` | bundle identity `gent.cyber.em34xx`, the `.em34xx` document type, the local-network usage text, minimum macOS |
| `res/em34xx.png`, `res/profile.png` | 1024 px icon masters drawn by `tools/gen_macos_assets.py` from the em34xxw glyphs (original artwork); the build makes `.icns` files with `sips` and `iconutil` (`make_icns.cmake`) |
| `notices.txt` | third-party notices (toml++ only: the C++ runtime and the frameworks are part of macOS), shown in About |
| `finish_bundle.cmake` | after linking: checks that the executable has every architecture in `CMAKE_OSX_ARCHITECTURES`, signs the bundle ad hoc |

TLS uses the system's own frameworks: Network.framework for the connection
and Security.framework for trust and certificate details
(`src/tn5250/tls_apple.mm`, [tn5250.md](tn5250.md)). The app imports
system libraries and frameworks only (`otool -L`).

## Building

```sh
export VCPKG_ROOT=~/vcpkg
cmake --preset macos-release && cmake --build --preset macos-release
# build/macos-release/em34xx.app, arm64 + x86_64
```

`macos-release` and `macos-debug` build everything universal: the
`universal-osx` overlay triplet builds the vcpkg ports for both
architectures and macOS 11. toml++ builds with Meson, which cannot compile
for two architectures at once, so that port is built for one and a
universal build uses toml++ header-only (`TOML_HEADER_ONLY=1`). The
ordinary `debug` and `release` presets build for the Mac's own
architecture (faster, for development); their `arm64-osx`/`x64-osx`
overlays also target macOS 11. `EM34XX_FRONTEND_COCOA` (AUTO, ON, OFF)
switches the app off; it is never built elsewhere. The target is
`em34xx_app` (the SDL program already owns the name `em34xx`).

The build signs the app ad hoc so that it runs from the build folder. A
release is signed with a Developer ID by `tools/macos-release.sh`
([releasing.md](releasing.md)).

## One app, a window per terminal

On Windows a second session is a second `em34xxw.exe`. A Mac app runs
once and opens more windows, so here every window is a terminal of its
own: its own `Application` (machine, emulation thread, TN5250 bridge) and
its own profile. Several terminals run side by side in one process.

- **File > New Connection** (⌘N) and **File > Connect to Profile** (⌘O)
  ask on the window in front. The profile then opens in that window when
  it has none yet (the first window at start), otherwise in a new window,
  cascaded from the one in front. Opening a profile never replaces the
  profile of a running terminal (Windows did, after asking to save).
- A `.em34xx` file double-clicked in Finder or dropped on the Dock icon
  opens the same way, each file in its own window; several files open
  several windows. A file from outside the profiles folder is asked about
  first (below).
- A profile that is already open in a window: **Show Window** brings that
  window forward; **Open Another Session** opens a second terminal with
  the same profile (another device on the same systems).
- **File > Close Window** (⌘W) or the close button ends that terminal only,
  after asking about its edited profile: the sessions close and its
  battery-backed RAM is written. The app keeps running without windows,
  as Mac apps do; the Dock icon, the Dock menu (New Connection, Connect to
  Profile) or the File menu opens a terminal again.
- **Quit** (⌘Q), log-out and shutdown ask about every edited profile (its
  window comes forward for the question), then end every terminal as
  Close Window does.
- Windows can be merged into tabs (**Window > Merge All Windows**, or
  "Prefer tabs" in System Settings > Desktop & Dock); the Window menu
  lists the terminals.
- Settings (speed) and the keymap are global and apply to every terminal
  at once, as they would to every em34xxw on Windows after a restart.

Observed (2026-11-23, macOS 26.6.2, arm64, debug build): started with two
profiles on the command line, the first opened in the first window
(pub400.com:992, TLS 1.3, verified) and the second in a new window
(pub400.com:23); both reached "session connected" and showed their
sign-on screens. SIGTERM then ended both terminals and the app with exit
code 0, the NVRAM written. The window bookkeeping that cannot be scripted
without UI automation (sheets, Show Window / Open Another Session, Close
Window with an edited profile, tabs) is to be checked by hand.

Shared between the terminals of one app:

- the **battery-backed RAM file** of the model (`3477.nvram`): each
  terminal reads it when it starts and writes it when it ends, so the
  setup saved last wins, as with several em34xxw instances (hypothesis
  for a later change: a RAM file per profile);
- the **log** (`em34xx.log`), opened by the first terminal: the lines of
  all terminals go to it.

## Start-up

1. The user folder is created if needed and `settings.toml` read.
2. Profiles to open: those Finder or the Dock passes at launch, else the
   non-option arguments of the executable
   (`em34xx.app/Contents/MacOS/em34xx PROFILE.em34xx ...`; options starting
   with `-` are left to AppKit).
3. The first terminal starts as the first profile describes it (model, CRT
   type, scan lines), else as a new profile would.
4. **First start**: ROM images missing or damaged: the ROM dialog opens
   (app-modal, before any window); closing it without usable ROMs ends the
   program.
5. The global keymap (`keymap.toml`) is checked; a broken file is reported
   and the built-in **mac** map is used ([keyboard.md](keyboard.md)).
6. The first window opens (its last frame, else the zoom size centred).
   Then: the profiles from step 2 open (the first in this window, the
   others in new windows); else the startup profile (Settings) when set
   and present; else **Connect to Profile** as a sheet on the window.
   Cancel leaves the terminal idle and not connected.

Each terminal's `Application` gets the command line em34xxw builds
(`--model`, `--rom-dir`, `--display`, `--speed`, `--keymap`, `--nvram`,
`--unmapped log`, `--log info`, `--no-crt`), and the first one
`--log-file`. `EM34XX_LOG` replaces `info` (e.g.
`info,tn5250=debug,ui=debug`; `ui=debug` logs where each profile opens).
`EM34XX_USER_DIR` names another user folder (tests, portable use).

Connecting, Reconnect, Reboot and the CRT type changes work as in
em34xxw ("Connecting: power cycle first" and "Applying a profile's
terminal settings" in [frontend-win32.md](frontend-win32.md)).

## The user folder

```
~/Library/Application Support/em34xx/
  settings.toml        application settings (the same file as em34xxw's)
  keymap.toml          the global keymap (only once edited; else "mac")
  roms/                ROM images
  profiles/*.em34xx    connection profiles (the same format as em34xxw's)
  3477.nvram           battery-backed RAM of the terminal
~/Library/Logs/em34xx.log   the log of the last run (Console shows it)
```

With `EM34XX_USER_DIR` the log is in that folder. Profiles and keymaps
can be copied between em34xxw and em34xx.app unchanged. `settings.toml`'s
`[window]` placement is em34xxw's; the Mac keeps each first window's frame
in its own defaults (`NSWindow` frame autosave), and `zoom` is shared.

## Menus

The menu bar has the Mac layout; Command shortcuts replace em34xxw's
Ctrl+Shift accelerators. Command never reaches the terminal, so every
other key, Control and Option included, does.

| Menu | Item | What (em34xxw equivalent) |
| --- | --- | --- |
| em34xx | About em34xx | the standard About panel: version, user folder, TLS backend, project page, notices (Help > About) |
| em34xx | Settings… (⌘,) | "Preferences…" before macOS 13; the Settings window (Edit > Settings) |
| em34xx | Keymap… | the keymap editor (Edit > Keymap) |
| em34xx | Hide, Hide Others, Show All, Services, Quit (⌘Q) | standard |
| File | New Connection… (⌘N) | (File > New Connection) |
| File | Connect to Profile… (⌘O) | the profile manager (File > Connect to Profile) |
| File | Close Window (⌘W) | ends the terminal in front (File > Exit for its window) |
| File | Save (⌘S), Save As… (⇧⌘S) | (File > Save, Save As) |
| File | Connection Settings… (⌘I) | the running profile in the editor (Edit > Connection Settings): on the Mac the Edit menu is for editing text |
| File | Disconnect / Reconnect (⌘D) | (File > Disconnect / Reconnect) |
| Edit | Undo, Redo, Cut, Copy, Paste, Select All | for the text fields of the dialogs |
| View | Actual Size (⌘0), Zoom 1.5×, Zoom 2×, Zoom In (⌘+), Zoom Out (⌘-) | (View > Zoom) |
| View | Show Toolbar, Customize Toolbar…, Enter Full Screen (⌃⌘F) | standard; in full screen the picture is letterboxed in black |
| Terminal | Attn, SysRq, Reset, Setup, Reboot (Power Cycle) | (Terminal menu) |
| Window | Minimize, Zoom, the terminals, Bring All to Front | standard |
| Help | em34xx Project Page (when set), Show Log in Finder | |

Commands are disabled while a sheet or dialog is up.

## Window, toolbar and status bar

- **Title**: the profile name ("em34xx" without one); **subtitle**: the
  model ("IBM 3477") and the front end's status note. An edited profile
  shows the dot in the close button instead of em34xxw's `*`; a saved
  profile is the title's proxy icon (⌘-click shows its folder).
- **Toolbar**: unified, customisable (View > Customize Toolbar), SF
  Symbols. Default items: New Connection, Profiles, Save, Disconnect /
  Reconnect (a toggle: picture, label and tooltip follow the state),
  Reboot, the terminal keys Attn, SysRq, Reset and Setup as a group of
  text buttons, Keymap. Settings is in the palette; on the Mac it lives in
  the app menu (⌘,).
- **Status bar**: "Profile: name" (with "— Edited") on the left; per
  session an orb and a lock on the right, with the same colours, states
  and tooltips as em34xxw's (green connected, orange connecting or waiting
  on a certificate prompt, red disconnected or failed, grey not
  configured; closed lock TLS, dimmed closed lock TLS without verification,
  open lock plain telnet).

## Dialogs

Dialogs that belong to a terminal are **sheets** on its window (stacked on
each other where one opens another: the profile manager's editor, the
editor's TLS Settings). The code runs each to completion
(`run_sheet()`, `run_alert()`) so it reads like em34xxw's; picture updates
and timers keep running meanwhile.

- **Connect to Profile**: the list (double-click connects); `+` (new
  profile: name, then the editor), `−` and an action menu (also the
  list's context menu): Edit…, Rename…, Duplicate (em34xxw: Copy), Move to
  Trash, Show in Finder. **Move to Trash** is not asked about: the profile
  can be put back from the Trash (em34xxw deletes, after asking).
- **Profile editor**, **TLS Settings** (Fetch runs off the main thread;
  the sheet may close first), **name prompt**: as em34xxw's, laid out with
  Auto Layout.
- **Settings** is a window of its own, not a sheet: startup profile,
  speed, default TLS minimum, ROM Images…, the user folder (Show in
  Finder). Changes apply and are saved at once, with no OK or Cancel, as
  Mac settings windows do. There is no file association button: the
  bundle declares the `.em34xx` type (`gent.cyber.em34xx.profile`,
  conforming to plain text) and macOS registers it when the app is first
  seen.
- **ROM Images**: as em34xxw's, plus files dropped on the list are
  imported; Show in Finder for the ROM folder.
- **Keymap** and **Key Mapping**: as em34xxw's; Mac key names in the
  capture box (Control, Option), Command is never captured. **Restore
  Defaults** restores the **mac** map.
- **Untrusted Certificate**: an alert with the certificate details and
  Stop (the default), Connect, Trust Permanently.
- **Certificate Not Valid**: a critical alert listing every error and the
  certificate, with Cancel (the default) and Connect Anyway, enabled only
  by "I understand the risk and want to connect to this server anyway".
  Never remembered.
- **Profiles from outside** the profiles folder (Finder, the Dock, the
  command line): a warning with the file, each session's host:port and
  how its certificate is checked; Cancel is the default.

The trust prompts never open over another sheet or dialog: the request
waits (orange orb) until it is gone, as in em34xxw. Disconnect, a new
connection, Close Window and Quit answer a waiting request with Stop.

## Picture

The emulator's display callback dispatches to the main queue (one block
in flight); the raster is fetched at most every 15 ms (a one-shot timer in
the common run-loop modes, so it also runs while a sheet is up), turned
into the tube image by `CrtRenderer` at the view's **backing pixel size**
(twice the points on a Retina screen, so the scan lines stay sharp) and
only the changed rows are redrawn. The view draws the 0xAARRGGBB buffer in
place as an sRGB `CGImage` without interpolation, black around it.
Moving to a screen with another scale re-renders.

View > Zoom sizes the window so the picture is the presented frame times
the zoom in **points** (the Windows build multiplies by DPI/96 instead),
shrunk to the screen when it does not fit; the top-left corner stays.
Manual resizing letterboxes.

## Keyboard

`keyDown:`/`keyUp:` give `NSEvent.keyCode` (`kVK_*`), turned into USB HID
usages by position (`hid_from_keycode()`); modifiers come from
`flagsChanged:` with the device-dependent bits telling left from right.

- macOS swaps the codes of the key left of 1 and the key left of Z on ISO
  keyboards; the table swaps them back (`KBGetLayoutType`).
- The Help key of Apple's extended keyboards is Insert (same position);
  keypad Clear is Num Lock; F13–F20 are F13–F20.
- Caps Lock reports each toggle once: it is sent as one press.
- Auto-repeats are dropped (the terminal does its own typematic).
- Command is never sent. Keys pressed with Command go to the menus, and
  when Command goes down every key the terminal thinks is held is
  released (macOS sends no key-up for keys released while Command is
  held). Losing the window's focus or the app's activation releases them
  too.
- No text input system: keys reach the terminal as keys, dead keys and
  input methods do not apply.
- Mac keyboards have no Right Ctrl (PCOMM's Enter): the default keymap is
  **mac**, where the key labelled *enter* (keypad Enter, fn+Return on
  laptops) is Enter and Return stays Field Exit. Keys the system keeps
  (Mission Control's Control+arrows, F11 Show Desktop when set so) do not
  reach the terminal; change them in System Settings > Keyboard or map
  other keys.

## Ending

Quit, log-out, shutdown and closing the last windows go through
`Application::request_quit()` and `finish()` for each terminal, so the
battery-backed RAM is written; sudden and automatic termination are off.
SIGTERM ends the app as a quit without questions. A terminal that asks to
end (the emulator's quit request) closes its window without questions.

## Not done / known gaps

- Sound: no `AudioSink` yet (the alarm is silent), as in em34xxw.
- Copy and paste of the screen text: not in em34xxw either.
- Signing with a Developer ID and notarisation are release steps
  (`tools/macos-release.sh`); CI builds are signed ad hoc only, and
  Gatekeeper refuses them on another Mac until allowed in System Settings
  > Privacy & Security.
- Not sandboxed (no Mac App Store build).
- Not yet tested by hand on macOS 11–15 or on an Intel Mac (the x86_64
  slice was run under Rosetta for `em34xx-headless --version` only).
