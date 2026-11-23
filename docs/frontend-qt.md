# em34xx-qt: the Qt front end

`em34xx-qt` is the GUI for Linux and FreeBSD: a Qt 6 Widgets main window
with a menu bar and a status bar around the terminal's screen. Like
`em34xxw` and `em34xx.app` it links `em34xx_runtime` only (no monitor, no
GDB stub). It is being built in slices; [frontend-win32.md](frontend-win32.md)
is the specification the later slices follow.

## Status

First slice: the picture, the keyboard, the menus below and the status bar.
It takes the same command line as `em34xx` (model, ROM directory, display,
keymap, `--connect`...) without the tooling options. Not yet there:
connection profiles, the ROM manager, Settings, the keymap editor, the TLS
trust prompts, the toolbar and saved window settings.

The icon is em34xxw's pixel art (`tools/gen_linux_assets.py` writes the PNGs
into `src/frontend/qt/res/hicolor/`): 16, 24, 32, 48 and 64 px are their own
drawings, and 96 to 512 px are whole-number enlargements, so no size is
smoothed. The sizes up to 256 px are built into the executable as the window
icon. `cmake --install` puts `em34xx-qt` in `bin`, `em34xx.desktop` in
`share/applications` and the PNGs in `share/icons/hicolor`; the desktop
entry is what a Wayland desktop takes the icon from. The desktop entry does
not yet register `.em34xx` profiles (the front end cannot open them).

| Menu | Item | What |
|------|------|------|
| File | Exit | closes the window |
| View | Zoom > 1x / 1.5x / 2x | the picture area becomes the presented frame times the zoom (logical pixels), shrunk to 90% of the desktop if it does not fit; resizing by hand stays possible |
| Terminal | Attn, SysRq, Reset, Setup | presses the terminal's own key (`Machine::terminal_key`) |
| Terminal | Reboot (Power Cycle) | `Application::power_on()` |
| Help | About... | version, licence (GPL-3.0-or-later), Qt version and platform |

The status bar shows the front end's status note on the left and the run
state (Running / Stopped) on the right.

## Build

`EM34XX_FRONTEND_QT` = `AUTO` (default: built on Linux and the BSDs when Qt
6.4 or later is found), `ON` or `OFF`. Qt is the system's (`qt6-qtbase-devel`
on Fedora, `qt6-base-dev` on Debian and Ubuntu), found with
`find_package(Qt6 COMPONENTS Widgets)`; it is not a vcpkg dependency.

There is no moc, uic or rcc step: the front end declares no signals or
slots (no `Q_OBJECT`), connects to lambdas and overrides event virtuals.

| File | What |
|------|------|
| `src/main_qt.cpp` | `Application(make_qt_frontend(), nullptr)`: `start()`, an error box if that fails, `run()` |
| `src/frontend/qt/qt_frontend.*` | the window, menus, status bar, picture and keyboard |
| `src/frontend/qt/qt_keys.*` | native scan code to USB HID usage |

`QApplication` is created in `Frontend::run()`, after the command line has
been handled, so `--help` and configuration errors need no display. Qt gets
the program name only, not the command line: its own options (`-platform`,
`-style`) are set through the environment (`QT_QPA_PLATFORM`,
`QT_STYLE_OVERRIDE`).

## Starting and stopping

**No display.** Without a usable platform plugin (no `WAYLAND_DISPLAY` or
`DISPLAY`, a display that cannot be reached, a bad `QT_QPA_PLATFORM`) the
`QGuiApplication` constructor does not return an error: it calls `qFatal`,
which ends the process with `abort()` (documented: QGuiApplication,
qFatal). Qt has no call that asks first, so `run()` forks a child that
constructs a `QGuiApplication` and exits; the child's message handler sends
Qt's messages back through a pipe and leaves with `_exit` on the fatal one,
so nothing aborts and nothing dumps core. If the child fails, `run()`
returns false with those messages and the three environment variables in
the error, and the program ends with exit code 2 through the normal exit
path. The cost is that the platform plugin is initialised twice on every
start.

**Signals.** SIGINT (Ctrl+C in the terminal it was started from) and
SIGTERM (kill, the end of the session) set a flag; the housekeeping timer
sees it within 100 ms, closes every window and leaves the event loop, also
from inside a dialog. `run()` then returns as it does when the window is
closed, and `Application` saves the battery-backed RAM. The previous
handlers are restored when `run()` returns.

**Errors before the window.** A bad option or missing ROM images end the
program in `Application::start()`, which writes the error to standard
error. When standard error is not a terminal (a desktop launcher),
`qt_error_box()` also shows it in a message box, if there is a display.
From a terminal there is no box. An error from `run()` itself (no display)
can only go to standard error.

## Picture

`Emulator::set_display_callback()` (emulation thread) queues one call to the
UI thread at a time (`QMetaObject::invokeMethod`, queued). The callback
does not hold the widget: it holds a shared `ScreenLink` (a mutex and the
widget pointer) and uses the pointer only under the mutex. Before the
window is destroyed the UI thread clears the callback and then, under the
mutex, the pointer; that waits for a call the emulation thread is in the
middle of making, and a call already queued finds no widget. The screen widget
then fetches the raster at most every 15 ms (`render_presented` under the
machine lock) and has `CrtRenderer` draw the tube at the widget's size in
device pixels (logical size times `devicePixelRatioF()`), so the scan-line
structure is computed at the output resolution. Only the rows that changed
are repainted. `paintEvent` draws the image 1:1 in device pixels, centred on
black. A 100 ms timer updates the title and status bar, honours quit
requests, and checks the display generation as a fallback.

## Keyboard

Positions, not characters: `QKeyEvent::nativeScanCode()` is mapped to a USB
HID usage by `qt_keys.cpp`. On Qt's `xcb` and `wayland` platforms the native
scan code is an XKB keycode, which with the evdev keycode set is the
kernel's input event code plus 8 (documented: xkeyboard-config `evdev`
keycodes); the table is keyed by event code. Auto-repeat events are dropped.

While the screen has the focus every key goes to the terminal: the widget
accepts `ShortcutOverride`, so Alt does not open the menus and no shortcut
fires, and it handles key events before `QWidget::event` so Tab does not
move the focus. Menus are reached with the mouse. Keys that are down are
released when the screen loses the focus and before a menu command runs.

Facts and their standing:

- observed (Fedora 45, GNOME, Qt 6.11.2): the window, menus, picture and
  status bar on the `wayland` and `xcb` platforms; the style is Fusion and,
  on Wayland, the frame is drawn by Qt's Adwaita decoration plugin.
- observed (same system, `QT_QPA_PLATFORM=offscreen`): SIGINT and SIGTERM
  end the program with exit code 0 and the `--nvram` file written; with
  `QT_QPA_PLATFORM=bogus`, with `DISPLAY=:97` on `xcb`, and with no display
  variables and no Wayland socket, the program reports the error and ends
  with exit code 2, without `abort()`; a bad option with standard error
  redirected opens the message box.
- hypothesis, not yet tested: the shutdown of the display callback under
  load (the race it closes was not reproduced); the fork in a process whose
  emulation thread is already running (the child uses only its own thread
  and `_exit`); the key table against a real keyboard; a
  non-evdev X server (keycodes would differ); FreeBSD.
- open: the compositor keeps its own shortcuts (Super, Alt+Tab, Alt+F4...).
  Whether Qt can ask a Wayland compositor to give them up has not been
  established.
