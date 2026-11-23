# Architecture

```
   Frontend (main thread)            AppTooling: monitor (replxx thread / rc / stdin)
   SDL3 or Win32 window                   |          ^
        |  frame-ready, render, keys      v          | qRcmd
        v                                                       GDB stub
   Application (config, HostLink, power, settings)              (thread)
        |                                                        |
        v                                                        v
              +-------------------- Emulator --------------------+
              |  run control, pacing, stop events, machine lock, |
              |  display generation                              |
              +--------------------------+----------------------+
                                         | emulation thread, 2000-instruction slices
                                         v
            Machine: I86 CPU  <->  Bus  <->  devices (model-specific)
                                    |             |
                              RAM / ROM      Twinax link device  <->  TwinaxHost
                                                                     (none / script / tn5250)
```

## Libraries

Each directory belongs to one CMake library, and a library links only the
layers below it. `cmake/check_layers.cmake` (the `layering` test) fails when
an `#include` crosses the layering.

```
em34xx_emu        util, cpu, bus, rom, machine   (Machine, Emulator, Cpu, interfaces)
  ^-- em34xx_net        net                      sockets
  ^-- em34xx_machines   devices, models, input   terminal hardware, model registry
  ^-- em34xx_host       host (minus the bridge)  Twinax hosts, workstation controller
  |     ^-- em34xx_tn5250   tn5250 (+ net)       TN5250 client, TLS
  |     ^-- em34xx_bridge   host/wsc/tn5250_bridge  controller + TN5250 sessions
  ^-- em34xx_present    present                  CRT rendering, display profiles
em34xx_runtime    config, app                    Application, HostLink  (-> machines, bridge, present)
  ^-- em34xx_devtools   monitor, gdb, devtools   monitor, replxx console, GDB stub, probes
  ^-- em34xx_frontend_sdl  frontend/sdl          SDL3 front end
  ^-- em34xx_frontend_win32 frontend/win32       Win32 front end (WIN32 only)
```

Front ends may include everything the runtime links (models, input, the
TN5250 client's headers) but never the tooling.

Executables, one per front end (names are CMake variables `EM34XX_EXE_*`):

| Executable | Links | Notes |
| --- | --- | --- |
| `em34xx` | frontend_sdl, devtools, runtime | only when SDL3 is found (`EM34XX_FRONTEND_SDL`) |
| `em34xx-headless` | devtools, runtime | always built; no SDL |
| `em34xxw` | frontend_win32, runtime | WIN32 only, GUI subsystem, end-user build; see [frontend-win32.md](frontend-win32.md) |
| `em34xx.app` (future) | frontend_cocoa, runtime (devtools optional) | APPLE only, MACOSX_BUNDLE, end-user build |

End-user GUI builds expose only the emulator (display, keyboard, sound,
settings) and connect/disconnect to a TN5250 host. Their user interface
offers no monitor console, GDB stub, probes, memory tools or screenshots.
They **may** still link `em34xx_devtools` (for example to enable a GDB stub
from the command line or a config file when diagnosing a problem); the
requirement is only that the GUI does not surface it. The runtime libraries
must not *need* the dev tooling: the `runtime_only` test links a minimal
application against `em34xx_runtime` alone to keep that true.
The core keeps its debug hooks generic and optional (breakpoints,
watchpoints, `Machine::trace_hook`, the unmapped-access log); nothing in
`em34xx_emu`..`em34xx_runtime` refers to the monitor or the GDB stub.

## Application

`src/app/application.hpp` wires configuration -> ROMs -> machine -> Twinax
host or TN5250 bridge -> run loop -> shutdown and exit code; each `main()`
is three lines. Developer tooling plugs in as an `AppTooling`
(`src/devtools/dev_tools.hpp`): it starts the GDB stub, runs the `--rc`
script, decides whether the CPU starts (`-S`, `--gdb-wait`), owns the main
thread when headless and runs the console thread beside a window. Without
it, tooling options are refused.

The control API a GUI calls directly: `host_link()` (`HostLink`:
connect/disconnect, per-station disconnect, controller status as text and
per session (`sessions()`), the TLS trust handler; the monitor's
`connect`/`disconnect` are wrappers over it), `power_on()`,
`power_off()`, `reset()`, `set_speed_percent()`, `set_crt()`, `config()`;
`connect_after_power_cycle()` (close the sessions, reset, connect: the
terminal runs its POST while the controller probes the line) and
`set_display_subtype()` (picture only, or, when
`Model::display_change_needs_power_cycle()` says the firmware or the host
would see it, `rebuild_machine()`: a new `Machine` built from the ROM files
with the new `TerminalOptions`, swapped in with `Emulator::replace_machine()`,
battery RAM written first so the new machine reads it back).
`last_error()` holds the reason `start()` failed, for front ends without a
console. Front ends that must own the process run loop (Cocoa) call `start()`,
drive the emulator from their loop, then `request_quit()` and `finish()`
instead of `main()`.

## Front ends

A front end implements `Frontend` (`src/frontend/frontend.hpp`):

- `run(Application&, FrontendOptions, error)`: the UI loop on the main
  thread; return when the window closes, `Emulator::quit_requested()` or
  `request_quit()`. Return false with an error if the window system cannot
  start.
- `request_quit()`, `set_title()`, `set_status()`: safe from any thread.

What it uses:

- **Picture.** Do not poll. `Emulator::display_generation()` moves when the
  picture may have changed; `wait_display_change(seen, ms)` blocks on it,
  and `set_display_callback()` is called (emulation thread, no machine lock)
  each time it moves: post a message to the UI thread (`PostMessage` /
  `InvalidateRect` on Win32, `dispatch_async` + `setNeedsDisplay:` on Cocoa,
  a user event on SDL). Then, under `Emulator::with_machine`,
  `render_presented()` (`present/display_profile.hpp`) gives the raw frame
  with the display subtype applied; `crt_fit()` and a `CrtRenderer`
  (`present/crt_render.hpp`; it recomputes only the rows a changed raster
  line reaches) turn it into the tube image at the window's pixel size.
  Redraw also on resize/expose. The generation moves after almost every
  slice (display register writes count), so fetch the raster at most once
  per refresh (the SDL front end: every 15 ms) and present only when the
  tube image changed.
- **Keyboard.** `Machine::key_event(HostKeyEvent)` under `with_machine`,
  with the key's **USB HID usage** (Keyboard/Keypad page 0x07; SDL
  scancodes are these values), `pressed` true/false, no host auto-repeat
  (drop repeats: the terminal does its own typematic). A native front end
  maps its key codes to HID usages with a table: Win32 from the scan code
  in `lParam` bits 16-23 plus the extended bit 24 (set 1 make codes, e.g.
  0x1E -> 0x04 `A`, 0x1C -> 0x28 Enter, E0 1C -> 0x58 keypad Enter);
  Cocoa from `NSEvent.keyCode` (kVK_* virtual key codes, e.g. kVK_ANSI_A
  0x00 -> 0x04, kVK_Return 0x24 -> 0x28), with modifier keys from
  `flagsChanged:`. Positions, not characters: the host keymap
  (`src/input/host_keymap.hpp`) maps HID usages to the terminal's keys.
- **Terminal keys.** `Machine::terminal_key("attn", pressed)` presses the
  terminal's own key for a function (set-3 position plus the terminal
  Shift/Alt its legend needs), bypassing the host keymap: toolbar buttons
  for Attn, SysRq, Reset, Setup. `Machine::set_keymap(spec)` replaces the host
  keymap at run time.
- **TLS trust.** `HostLink::set_tls_trust_handler()`: when TLS is on and
  the configuration refuses the server certificate (verification failed,
  or not the pinned one), the handler gets a `tn5250::TlsTrustRequest`
  (host, port, station, certificate details, reason) on the session's I/O
  thread and returns Reject, AcceptOnce or AcceptPermanently; it may block
  while the UI asks. The front end records a permanent decision itself
  (em34xxw pins the fingerprint in the profile). It must answer pending
  requests before it disconnects (the session threads are joined). The
  layers: `tn5250/tls_trust.hpp` (types), `Transport::set_trust_handler()`
  and `tls_accept_peer()` (every backend), `tn5250::Client`,
  `wsc::BridgeOptions::tls_trust`, `TlsTrustBroker` in `app/host_link.hpp`
  (per-bridge memory of accepted certificates). Without a handler (SDL,
  monitor) the configured behaviour is unchanged. Details in
  [tn5250.md](tn5250.md).
- **Sound.** `Machine::set_audio_sink(AudioSink*)` (`machine/peripherals.hpp`):
  `tone(on, hz)`, `speaker(on)`, `click()` arrive on the emulation thread
  under the machine lock; queue them for the audio thread.
- **Control.** The Application API above.

To add `em34xx.app` (as `em34xxw` was): a `em34xx_frontend_<name>` library
linking `em34xx_runtime`, a `main` that constructs the front end and
`Application(frontend, nullptr)`, and an executable target guarded by
`if(APPLE)`; `CMakeLists.txt` has a commented stub. End-user data
(settings, profiles, the user folder, ROM status, keymap files as text) is
platform independent in `config/` (`profile.hpp`, `settings.hpp`,
`user_dirs.hpp`) and `input/host_keymap.hpp` for the next GUI to reuse.

## Principles

- **Models describe hardware, not firmware.** A model maps memory and
  devices. Every fact it encodes is either observed (board, datasheet,
  documentation) or marked as a hypothesis in `ModelInfo::notes`.
- **No ROM patches, no ROM-derived code.** Firmware runs unmodified; the
  emulator contains nothing copied from it.
- **Fail loudly on the unknown.** Accesses the model does not decode stop the
  machine by default, so each unknown piece of hardware becomes a concrete
  task. The `unmapped` report aggregates them; probe devices stand in while
  the behaviour is worked out.
- **One CPU interface, several levels.** `CpuLevel` selects 8086/8088
  quirks and 80186 instructions in the I86 core; 80286 protected mode will
  be a core of its own behind the same `Cpu` interface.

## CPU

`src/cpu/i86.cpp` is an instruction-level interpreter. It is validated
against SingleStepTests 8088 V2. Cycle counts are approximate: deterministic,
and good enough for pacing and timers, but they do not model the prefetch
queue. A REP string instruction is restartable: if an interrupt is pending
between iterations, IP rewinds to the prefix.

### Extension points for the 80286 (3489)

Nothing of the 286 is implemented (no ROM dump to bring it up against).
What is in place so a protected-mode core slots in:

- `cpu/cpu.hpp`: the `Cpu` interface the Machine, monitor and GDB stub use
  instead of the concrete `I86`; `make_cpu()` picks the core per
  `CpuLevel` (the place to return the 286 core for `I80286`).
- Debugger address translation: `Cpu::translate(seg, off, linear)` is
  segment:offset in real mode and selector:offset through the descriptor
  tables in protected mode; the monitor's `seg:off` expressions, its
  disassembly and the GDB stub's `eip` writes go through it
  (`debug_linear()` falls back to real-mode arithmetic for display).
- `SystemRegs` (MSW, GDTR, IDTR, LDTR, TR and the segment descriptor
  caches) via `Cpu::system_regs()` / `protected_mode()`. The monitor's
  `regs` prints them when present; the GDB stub then appends an
  `org.em34xx.i286.system` feature (msw, gdtr/idtr base and limit, ldtr,
  tr as registers 32..38, read-only).
- 24-bit addressing: `ModelInfo::address_bits` sizes the Bus page table
  (65536 pages for 16 MiB), the breakpoint bitmap (2 MiB) and the CPU's
  address mask; breakpoints and watchpoints are on linear addresses, which
  are physical before the 80386.
- Still to do with the core: the monitor's register assignment (`r msw`),
  breakpoints given as selector:offset, and an expression syntax for
  protected-mode addresses.

## Bus

`Bus` decodes memory in 256-byte pages. The CPU uses byte accesses; word
accesses are split by the CPU according to its bus width. Debugger access
(`peek8`/`poke8`) has no side effects and ignores policy and watchpoints.
The monitor can write RAM only, never ROM.

## Threads

The machine itself is single-threaded. The Emulator runs it in slices on its
own thread and releases the machine lock between slices. The monitor, GDB
stub and front end take the lock briefly through `Emulator::with_machine`.

Display change notification: the model tells the bus which pages hold its
picture (`Bus::observe_writes`: the 3477's cell windows, character
generator and display registers), and the display device reports an
`animation_phase()` (the blink phase). After every slice, single step and
`with_machine` call the Emulator asks `Machine::take_display_change()` and,
if anything changed, bumps `display_generation()`, wakes
`wait_display_change()` and calls the display callback.

Pacing: after each slice the emulation thread compares the CPU cycles run
since the start of the run with the wall clock (`steady_clock`) and sleeps
until the machine is back on its nominal clock, in steps of at most 20 ms,
skipping sleeps under 1 ms (sleep granularity, coarse under WSL; the
absolute target absorbs oversleep). More than 250 ms behind (a debugger
pause, a slow build) it resynchronises instead of racing. Device time
(timers, keyboard typematic, display frames) is derived from CPU cycles, so
the terminal's own timing holds whatever the host speed. `--log perf=debug`
reports the achieved clock, the sleeps, and in the SDL front end the
display changes, raster fetches and CRT render cost.

## Twinax / TN5250

`src/host/twinax_link.hpp` fixes the unit both sides exchange: the 16-bit
5250 workstation frame (sync, data LSB-first, 3-bit station address with 7 as
end-of-message, even parity, fill). The model's link device, still unknown,
will turn firmware register accesses into frames. A `TwinaxHost` plays the
controller. The TN5250 client (`src/tn5250`, see `docs/tn5250.md`) keeps
the 5250 shadow screen and does the operator field editing; the virtual
workstation controller (`src/host/wsc`) drives the station from it through
`tn5250::DisplaySession`.
