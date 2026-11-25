# em34xx-qt: the Qt front end

`em34xx-qt` is the end-user build for Linux and FreeBSD: a Qt 6 Widgets
window with menus, a toolbar and a status bar, connection profiles, a ROM
manager, a keymap editor, the TLS trust prompts and screen burn-in. It has
the functions and the layout of [em34xxw](frontend-win32.md), which is its
specification; this document describes where Linux differs and what has
been tested. Like `em34xxw` and `em34xx.app` it links `em34xx_runtime`
only (no monitor, no GDB stub; use `em34xx` / `em34xx-headless` for those).

One process is one terminal, as on Windows: a second session is a second
`em34xx-qt`.

Sources: `src/main_qt.cpp`, `src/frontend/qt/`:

| File | What |
| --- | --- |
| `qt_frontend.*` | launch (command line, display check, user folder, settings, first-start ROM check), the main window, picture, keyboard, profiles and connections, the TLS trust queue, signals and the end of the desktop session |
| `qt_dialogs.*` | ROM images, Settings, profile manager and editor, TLS Settings, the trust prompt, the invalid-certificate prompt, Keymap and Key Mapping, About, the file association |
| `qt_util.*` | UTF-8/QString, message boxes, the exception guard for menu commands, the user folder, the glyphs, opening files and folders |
| `qt_keys.*` | native scan code to USB HID usage |
| `res/` | the icon (`hicolor/`), em34xxw's toolbar and status glyphs as PNGs (`glyphs/`), the document icon, `em34xx.desktop`, the `.em34xx` file type (`em34xx-mime.xml`) and the third-party notices (`notices.txt`) |

## Build and install

`EM34XX_FRONTEND_QT` = `AUTO` (default: built on Linux and the BSDs when Qt
6.4 or later is found), `ON` or `OFF`. Qt is the system's (`qt6-qtbase-devel`
on Fedora, `qt6-base-dev` on Debian and Ubuntu), found with
`find_package(Qt6 COMPONENTS Widgets)`; it is not a vcpkg dependency.

There is no moc or uic step: the front end declares no signals or slots
(no `Q_OBJECT`), connects to lambdas and overrides event virtuals. rcc
embeds the files under `res/` in the front-end library, so the window
icon, the glyphs, About's notices and the file association work without an
installed icon theme or data files.

`cmake --install` puts `em34xx-qt` in `bin`, `em34xx.desktop` in
`share/applications`, the file type in `share/mime/packages/em34xx.xml`
and the icons (application, `apps/em34xx.png`; profile document,
`mimetypes/application-x-em34xx-profile.png`) in `share/icons/hicolor`. The
desktop entry is what a Wayland desktop takes the window's icon from.

The artwork is em34xxw's (original): `tools/gen_linux_assets.py` writes the
application icon (16 to 64 px its own drawings, 96 to 512 px whole-number
enlargements, so no size is smoothed), the toolbar and status glyphs at 16,
24, 32 and 48 px, and the document icon.

## Start-up

As em34xxw's ("Start-up" in [frontend-win32.md](frontend-win32.md)):

1. The command line: `em34xx-qt [PROFILE.em34xx]`, plus `--help` and
   `--version` (the emulator's own options are not taken: they belong to
   the developer build, `em34xx`). Anything else is an error, exit code 2.
   Qt gets the program name only: its own options (`-platform`, `-style`)
   are set through the environment (`QT_QPA_PLATFORM`, `QT_STYLE_OVERRIDE`).
2. **The display.** Without a usable platform plugin (no `WAYLAND_DISPLAY`
   or `DISPLAY`, a display that cannot be reached, a bad
   `QT_QPA_PLATFORM`) the `QGuiApplication` constructor does not return an
   error: it calls `qFatal`, which ends the process with `abort()`
   (documented: QGuiApplication, qFatal). Qt has no call that asks first, so
   a child process constructs a `QGuiApplication` and exits; its message
   handler sends Qt's messages back through a pipe and leaves with `_exit`
   on the fatal one, so nothing aborts and nothing dumps core. If the child
   fails, the error (with the three environment variables) goes to standard
   error and the program ends with exit code 2. The cost is that the
   platform plugin is initialised twice on every start.
3. The user folder (below) is created if needed and `settings.toml` read.
4. The terminal starts as the profile that will be opened describes it,
   else as the first model whose ROM images are there; a profile for a
   model without its images is named with the missing files and the ROM
   dialog offered; with no model that can run, the ROM dialog opens before
   the window and closing it with none ends the program (exit code 1).
5. The global keymap (`keymap.toml`) is checked; a broken file is reported
   and the built-in PCOMM map is used.
6. The `Application` starts with em34xxw's command line (`--model`,
   `--rom-dir`, `--speed`, `--keymap`, `--nvram`, `--eeprom`, `--unmapped
   log`, `--log info --log-file <folder>/em34xx-qt.log`, `--no-crt`,
   `--phosphor`). `EM34XX_LOG` replaces `info` with any `--log` spec.
7. The window opens. A profile on the command line connects (asked about
   first when it is not in the profiles folder); otherwise the startup
   profile does when Settings asks for one; otherwise **Connect to
   Profile** opens, modal over the window.

Errors before the window (a broken settings file, missing ROM images)
appear in message boxes and on standard error.

## The user folder

`$XDG_DATA_HOME/em34xx` (`~/.local/share/em34xx` by default;
`QStandardPaths::GenericDataLocation`), or the folder named by
`EM34XX_USER_DIR` (portable use and tests). The layout is
`config/user_dirs.hpp`, shared with em34xxw and em34xx.app: profiles and
keymaps can be copied between them unchanged.

```
~/.local/share/em34xx/
  settings.toml        application settings (the same file as em34xxw's)
  keymap.toml          the global keymap (only once edited; else PCOMM)
  roms/                ROM images
  profiles/*.em34xx    connection profiles
  3477.nvram           battery-backed RAM of the terminal (its setup)
  tube.burn            the wear of the tube (docs/burn-in.md)
  em34xx-qt.log        the log of the last run
```

`settings.toml`'s `[window]` placement is the window's position and its
size without the frame. Wayland does not let a client place its window, so
there only the size and the maximised state are restored.

## Menus, toolbar and status bar

em34xxw's, item for item ("Menus and toolbar" in
[frontend-win32.md](frontend-win32.md)): File (New Connection, Connect to
Profile, Save, Save As, Disconnect / Reconnect, Exit), Edit (Connection
Settings, Keymap, Settings), View (Zoom 1x / 1.5x / 2x, Zoom In, Zoom Out;
Burn-in Off / Slow / Fast / Hyper, Replace CRT), Terminal (Attn, SysRq,
Reset, Setup, Reboot), Help (Show Log File, Open User Folder, About). The
accelerators are em34xxw's Ctrl+Shift chords (N, O, S, D, E); every other
key belongs to the terminal.

- **Toolbar**: em34xxw's buttons and glyphs (New Connection, Connect to
  Profile, Save; Disconnect / Reconnect, whose picture and tooltip follow
  the state, Reboot; Settings, Keymap; Attn, SysRq, Reset and Setup as text
  buttons). The buttons take no keyboard focus.
- **Title**: `<profile>[*] - IBM 3477 - em34xx`.
- **Status bar**: `Profile: <name>[*]` on the left; per session an orb and
  a lock on the right with em34xxw's colours, states and tooltips.
- **Zoom** sizes the picture area to the presented frame times the zoom in
  logical pixels (the desktop's scale factor applies, as DPI/96 does on
  Windows), shrunk to the screen when it does not fit.
- **Help > Show Log File** opens the log in the desktop's text viewer
  (em34xxw selects it in Explorer); **Open User Folder** opens the file
  manager.

## Dialogs

The dialogs are em34xxw's, laid out with Qt layouts, modal and centred on
their owner by Qt: ROM Images (with Show Folder), Settings (with Open User
Folder; OK and Cancel), Connect to Profile, the profile editor (a model
without its ROM images is grey and says so), TLS Settings (Fetch on a
worker thread; the dialog may close first), the name prompt, Untrusted
Certificate (Stop is the default), Certificate Not Valid (Cancel is the
default; Connect Anyway only after "I understand the risk..."), Keymap and
Key Mapping (click the capture box, then press the key; Tab is captured
too), About (version, user folder, TLS backend, Qt version and platform,
project page, notices).

The trust prompts never open over another dialog
(`QApplication::activeModalWidget()`): the request waits with the orb
orange until it has closed. Disconnect, a new connection and Exit answer a
waiting request with Stop; while sessions are closed a new request is
refused (the trust queue drains, as in em34xxw).

**File association**: Settings > "Open .em34xx files with em34xx-qt..."
asks first, then writes, for the user only, the file type
(`$XDG_DATA_HOME/mime/packages/em34xx.xml`, `application/x-em34xx-profile`,
glob `*.em34xx`) and a desktop entry (`$XDG_DATA_HOME/applications/
em34xx.desktop`) whose `Exec` is this executable, then runs
`update-mime-database`, `update-desktop-database` and `xdg-mime default`
when they are installed. An installed em34xx-qt registers the type through
its packaged files without this.

## Picture

`Emulator::set_display_callback()` (emulation thread) queues one call to the
UI thread at a time (`QMetaObject::invokeMethod`, queued). The callback
does not hold the widget: it holds a shared `ScreenLink` (a mutex and the
widget pointer) and uses the pointer only under the mutex. Before the
window is destroyed the UI thread clears the callback and then, under the
mutex, the pointer; that waits for a call the emulation thread is in the
middle of making, and a call already queued finds no widget. The screen
widget fetches the raster at most every 15 ms (`render_presented` under the
machine lock) and has `CrtRenderer` draw the tube, with the wear of
screen burn-in, at the widget's size in device pixels (logical size times
`devicePixelRatioF()`), so the scan-line structure is computed at the
output resolution. Only the rows that changed are repainted.

## Keyboard

Positions, not characters: `QKeyEvent::nativeScanCode()` is mapped to a USB
HID usage by `qt_keys.cpp`. On Qt's `xcb` and `wayland` platforms the native
scan code is an XKB keycode, which with the evdev keycode set is the
kernel's input event code plus 8 (documented: xkeyboard-config `evdev`
keycodes); the table is keyed by event code. Auto-repeat events are dropped.

While the screen has the focus every key goes to the terminal: the widget
accepts `ShortcutOverride` for everything but the window's Ctrl+Shift
accelerators, so Alt does not open the menus, and it handles key events
before `QWidget::event` so Tab does not move the focus. Keys that are down
are released when the screen or the window loses the focus and before a
menu command runs.

## Ending

Closing the window asks about an unsaved or changed profile; then the
tube's wear, the window placement and the settings are written, and
`Application` writes the battery-backed RAM.

**Signals.** SIGINT (Ctrl+C in the terminal it was started from) and
SIGTERM (kill) set a flag; a timer sees it within 100 ms and closes the
window without questions, also from inside a dialog. The previous handlers
are restored when the window has gone.

**End of the desktop session** (`QGuiApplication::commitDataRequest`, X11
session management): an unsaved or changed profile is asked about when
the session manager allows it, and Cancel asks the session to stay; then
the battery-backed RAM, the setup EEPROM, the tube's wear and the settings
are written at once (`Application::flush_persistent_state()`), as em34xxw
does on `WM_ENDSESSION`.

Exceptions do not cross Qt's event handlers: menu commands and the
dialogs' buttons catch them (`qt::guarded`) and report them in a message
box; the operation is abandoned and the program keeps running.

## Testing

`tests/qt_gui_test.cpp` runs the real launch, `Application` and window on
Qt's `offscreen` platform in a temporary user folder (and temporary
`XDG_DATA_HOME` and `XDG_CONFIG_HOME`), and plays a script from a timer
that also runs inside modal dialogs: each step waits for a window or
dialog, checks it, and clicks or fills it as a user would. `ctest -L qt`
runs the scenarios; they need the 3477 ROM images in `roms/`, like the ROM
smoke tests. `em34xx_qt_gui_test SCENARIO roms/ SHOT_DIR` also saves a
screenshot of each screen.

| Scenario | What |
| --- | --- |
| `first-start` | no ROM images: the ROM dialog before any window says one model's images are needed; closed, the error box, exit code 1 |
| `profiles` | Connect to Profile at start; New (name prompt, editor), Copy, Delete (asked); Connect (title, status bar, POST); Connection Settings marks the profile modified, Save clears it; Settings, Keymap and Key Mapping (Ctrl+F5 captured from a synthetic key event with its XKB keycode), About and its notices, ROM Images; the file association (desktop entry and file type written); Zoom; Disconnect turns the button into Reconnect; Exit writes the profile, NVRAM, keymap and window placement |
| `foreign` | a profile from outside the profiles folder: the question names the host and "TLS, certificate NOT VERIFIED", No is the default and leaves the terminal idle |
| `trust` | two local `openssl s_server`s: a self-signed certificate brings Untrusted Certificate (the orb orange meanwhile), Trust Permanently pins it in the saved profile; an expired one brings Certificate Not Valid with Connect Anyway disabled, and Cancel holds the session (no second prompt). Skipped without the `openssl` command |
| `burn-in` | hyper burn-in for some seconds, Replace CRT (asked), Burn-in Off saved; `tube.burn` written |

Facts and their standing:

- observed (2026-11-25, Fedora 45, Qt 6.11.2, `offscreen` platform): all
  five scenarios above pass, and their screenshots were looked at.
- observed (earlier slice, Fedora 45, GNOME, Qt 6.11.2): the window, menus,
  picture and status bar on the `wayland` and `xcb` platforms; the style is
  Fusion and, on Wayland, the frame is drawn by Qt's Adwaita decoration
  plugin. SIGINT and SIGTERM end the program with exit code 0 and the
  NVRAM written; a missing display is reported with exit code 2 without
  `abort()`.
- not tested: this version on a real display (Wayland or X11) by hand,
  among it typing into a session, the toolbar at other scale factors,
  window placement on X11, Show Log File and Open User Folder with a real
  desktop, and the file association in a real file manager; the end of the
  desktop session (`commitDataRequest`); a session against a real IBM i
  host (the tests use local servers); FreeBSD.
- hypothesis, not yet tested: the key table against every real keyboard;
  a non-evdev X server (keycodes would differ).
- open: the compositor keeps its own shortcuts (Super, Alt+Tab, Alt+F4...).
  Whether Qt can ask a Wayland compositor to give them up has not been
  established.

## Known gaps

As em34xxw's: no sound (the alarm is silent), no printer sessions, one
global keymap. The colour 3486/3487's monitor alignment is marked done by
the model (docs/bringup-3486.md).
