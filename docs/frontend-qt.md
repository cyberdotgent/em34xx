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
trust prompts, the toolbar, saved window settings, a `.desktop` file and
icon.

| Menu | Item | What |
|------|------|------|
| File | Exit | closes the window |
| View | Zoom > 1x / 1.5x / 2x | the picture area becomes the presented frame times the zoom (logical pixels), shrunk to 90% of the desktop if it does not fit; resizing by hand stays possible |
| Terminal | Attn, SysRq, Reset, Setup | presses the terminal's own key (`Machine::terminal_key`) |
| Terminal | Reboot (Power Cycle) | `Application::power_on()` |
| Help | About... | version, Qt version and platform |

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
| `src/main_qt.cpp` | `Application(make_qt_frontend(), nullptr).main()` |
| `src/frontend/qt/qt_frontend.*` | the window, menus, status bar, picture and keyboard |
| `src/frontend/qt/qt_keys.*` | native scan code to USB HID usage |

`QApplication` is created in `Frontend::run()`, after the command line has
been handled, so `--help` and configuration errors need no display. Qt gets
the program name only, not the command line: its own options (`-platform`,
`-style`) are set through the environment (`QT_QPA_PLATFORM`,
`QT_STYLE_OVERRIDE`).

## Picture

`Emulator::set_display_callback()` (emulation thread) queues one call to the
UI thread at a time (`QMetaObject::invokeMethod`, queued). The screen widget
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
- hypothesis, not yet tested: the key table against a real keyboard; a
  non-evdev X server (keycodes would differ); FreeBSD.
- open: the compositor keeps its own shortcuts (Super, Alt+Tab, Alt+F4...).
  Whether Qt can ask a Wayland compositor to give them up has not been
  established.
