# em34xx

A hardware-level emulator for IBM 34xx-family 5250 (Twinax) terminals,
running the terminals' own firmware. The terminals are iterations of one
design built around 8086-family CPUs (8088 → 80286), so one emulator covers
them with a per-model machine description.

Status: **scaffold / bring-up**. The CPU core is complete for the 8086/8088
(and has the 80186 extensions); the terminal hardware around it is still
being discovered.

| Model | Status | Notes |
| --- | --- | --- |
| 3477 | bring-up (primary) | clean ROM pair; runs from reset into hardware init |
| 3486 | ROM incomplete | only dump has A17 stuck high; re-dump requested |
| 3489 | placeholder | needs 80286 protected mode and a good ROM dump |

## Building

Requirements: CMake ≥ 3.21, Ninja, a C++17 compiler, and
[vcpkg](https://vcpkg.io) (dependencies: SDL3, replxx, toml++; tests add
doctest, nlohmann-json and zlib).

TLS for the TN5250 client uses the host's own TLS stack, not vcpkg: the
system OpenSSL (libssl/libcrypto, Debian/Ubuntu `libssl-dev`) on Linux, and
SChannel/CryptoAPI (system DLLs) on Windows. `-DEM34XX_TLS=AUTO` (the
default; the presets leave it alone) uses it when found and otherwise builds
without TLS, with a configure-time message; `ON` makes a missing TLS stack
an error, `OFF` builds without TLS.

```sh
export VCPKG_ROOT=~/vcpkg
cmake --preset release
cmake --build --preset release
```

The language level is deliberately C++17: the project must stay buildable
with toolchains that target Windows 7 (x86).

Executables (each front end is its own program; see
[docs/architecture.md](docs/architecture.md)):

| Program | Built when | What it is |
| --- | --- | --- |
| `em34xx` | SDL3 found (`-DEM34XX_FRONTEND_SDL=AUTO`, default; `ON` requires it, `OFF` skips it) | SDL3 window plus the developer tooling: monitor console, GDB stub, scripts; `--headless` runs it without a window |
| `em34xx-headless` | always | the same without SDL: monitor on stdin/stdout, scripts, GDB stub |
| `em34xxw` | Windows builds only | native Win32 GUI for end users: profiles, ROM manager, keymap editor; no developer tooling ([docs/frontend-win32.md](docs/frontend-win32.md)) |
| `tn5250-probe` | always | stand-alone TN5250 client for protocol tests |

A Cocoa `em34xx.app` is planned the same way, without the developer
tooling.

### Windows (MinGW-w64 cross build)

Windows 7 and later, 32-bit (x86, the primary Windows target) or 64-bit,
cross-built on Linux with MinGW-w64 (Debian/Ubuntu: `g++-mingw-w64-i686`,
`g++-mingw-w64-x86-64`; wine is optional and runs the tests):

```sh
export VCPKG_ROOT=~/vcpkg
cmake --preset mingw-x86-release      # or mingw-x86-debug, mingw-x64-*
cmake --build --preset mingw-x86-release
ctest --preset mingw-x86-release -LE slow   # through wine, if installed
```

The output is `build/mingw-x86-release/em34xxw.exe` (the GUI),
`em34xx.exe` and `em34xx-headless.exe` (console programs with the
developer tooling). All need only system DLLs: the MinGW runtime, SDL3 and the other ports are
linked statically. The toolchain files in `cmake/toolchains/` use the
compilers' posix thread model (`*-g++-posix`) and `_WIN32_WINNT=0x0601`;
the overlay triplets in `cmake/triplets/` build the vcpkg ports with them.
The monitor runs in the Windows console (replxx); `--headless` and
`--batch` work as on Linux.

### em34xxw (Windows GUI)

`em34xxw.exe` keeps everything in one folder per user, `%APPDATA%\em34xx`
(or the folder in the environment variable `EM34XX_USER_DIR`):

```
%APPDATA%\em34xx\
  settings.toml        CRT type and rendering, speed, default TLS minimum,
                       zoom, window placement
  keymap.toml          the global host keymap (Edit > Keymap; PCOMM until edited)
  roms\                ROM images (imported through the ROM dialog)
  profiles\*.em34xx    connection profiles
  3477.nvram           the terminal's battery-backed setup memory
  em34xxw.log          log of the last run
```

On the first start (no usable ROMs) a dialog lists what each model needs
(present / missing / bad hash) and imports dumps under any name: they are
recognised by SHA-256. Later the same dialog is under Edit > Settings.

File > Connect... connects without saving ("Untitled Profile*"); File >
Save As keeps it. File > Connect to Profile... manages the saved profiles
(New, Edit, Rename, Copy, Delete). `em34xxw PROFILE.em34xx` opens and
connects a profile; Settings can associate `.em34xx` files with em34xxw for
the current user. The toolbar has Attn, SysRq and Reset, sent as the
terminal's own keys. With TLS, a certificate that does not verify brings up
a prompt: Stop, Connect (this time) or Trust Permanently (its SHA-256 goes
into the profile).

A profile is TOML and never holds a password:

```toml
format = 1
model = "3477"

[tls]
enabled = true
verify = "system"          # "system", "none" (accept all) or "fingerprint"
cert_sha256 = ""           # the trusted SHA-256 with "fingerprint"
min_version = "1.2"        # optional

[[session]]                # session 1 (station 0)
host = "pub400.com"
port = 992
device_name = ""           # optional

[[session]]                # session 2 (station 1), optional
host = ""
port = 23
```

Details: [docs/frontend-win32.md](docs/frontend-win32.md).

## ROMs

ROM images are not distributed. Put them in `roms/` under the names listed in
[roms/ROMS.md](roms/ROMS.md); they are git-ignored and verified by SHA-256.
Known-defective dumps are refused unless `--allow-bad-rom` is given.

```sh
build/release/em34xx --check-roms --model 3477
```

The emulator is a clean implementation: it contains no code or data taken
from the ROMs, and it never patches them. Hardware behaviour is emulated
instead.

## Running

```sh
build/release/em34xx --model 3477              # window + monitor on the terminal
build/release/em34xx --headless -S             # monitor only, CPU stopped at reset
build/release/em34xx-headless -S               # the same, from the build without SDL
build/release/em34xx --batch --rc script.rc    # automated run; exit code from the script
build/release/em34xx --gdb -S                  # GDB stub on 127.0.0.1:1234
```

One ROM serves every display subtype and language of a model. Choose the
tube with `--display green|amber|paperwhite|color` (or the shorthands
`--green`, `--amber`, `--paperwhite`, `--color`). The keyboard decides the
terminal's language through its ID, so it is configured too:
`--keyboard 122|enhanced --keyboard-layout us` (see
[docs/keyboard.md](docs/keyboard.md)).

Configuration can also come from a TOML file (`--config`); see
[em34xx.example.toml](em34xx.example.toml). `--dump-config` prints the
effective configuration.

During bring-up, any access to an address or port that the model does not
decode stops the machine (`--unmapped stop`, the default) and is reported.
`--unmapped log` or `ignore` keep running instead.

### Monitor

Numbers are hex by default. Addresses are linear or `seg:off`.

```
g [addr]          run until a stop (Ctrl-C breaks in)
s/t [n]           step / trace n instructions
bp addr, wp addr  breakpoints and watchpoints
d, u, e           dump, disassemble, enter bytes
map, unmapped     memory map, unexpected accesses seen so far
map probe a len   stand-in device that logs accesses
assert expr msg   for scripts; failures give exit code 1
```

`help` lists every command; see [docs/monitor.md](docs/monitor.md).

### GDB

```sh
gdb-multiarch -x tools/em34xx.gdb -ex 'target remote :1234'
```

The stub describes an i8086 target. `eip` is the linear address of CS:IP,
and memory addresses are linear. `monitor <cmd>` runs any monitor command;
its output streams to GDB while the command runs.

[tools/em34xx.gdb](tools/em34xx.gdb) sets up 16-bit Intel-syntax
disassembly, shows `monitor regs` and the next instructions at every stop,
and adds segmented-address helpers (numbers are hex, as in the monitor;
`SEG:OFF` and register names also work):

```
lin  SEG OFF          linear address of SEG:OFF
bseg SEG OFF          breakpoint at SEG:OFF
useg SEG OFF [N]      disassemble N instructions at SEG:OFF
wseg SEG OFF [LEN]    write watchpoint on LEN bytes at SEG:OFF
tnc  [PORT]           target remote :PORT (default 1234)
```

When GDB stops with SIGBUS, the machine hit an unexpected access (see
`--unmapped`); `monitor policy log` keeps running past them.

## Host connection

The terminal will reach an IBM i host through TN5250. Data will enter the
firmware through an emulated Twinax link device, using the same registers
and interrupts the real hardware presents. So far only the configuration
surface exists (`--twinax-host`, `--tn5250-*`). `--twinax-host script` lets
the monitor inject 5250 frames (`twinax send`) for link-device bring-up.

## Tests

```sh
tools/fetch-test-vectors.sh            # ~760 MB, SingleStepTests 8088 V2 (MIT)
ctest --preset release                 # unit, ROM smoke (if ROMs present), CPU vectors
ctest --preset release -LE slow        # skip the CPU vectors
```

The CPU passes all 2,927,000 documented-opcode 8088 vectors. Registers,
memory and flags are compared, with flags masked where the vectors mark
them undefined.

## Layout

```
src/cpu        8086-family interpreter and disassembler
src/bus        memory/I/O decode, unexpected-access policy, probe devices
src/machine    Machine (CPU+bus+devices) and the threaded Emulator
src/models     per-model definitions (3477, 3486, 3489)
src/host       Twinax frame interface and TN5250 configuration (stub)
src/monitor    debug monitor and expression evaluator
src/gdb        GDB remote stub
src/frontend   SDL3 window
docs/          architecture and bring-up notes
roms/ROMS.md   expected ROM files and hashes
```

## Licence

em34xx is free software, licensed under the GNU General Public License,
version 3 or (at your option) any later version (`GPL-3.0-or-later`); see
[LICENSE](LICENSE). It contains no IBM code or data: the terminal firmware
ROMs are not part of this project and must be supplied by the user.

Dependencies are under GPL-compatible licences: SDL3 (zlib), replxx (BSD),
toml++ (MIT), and for tests doctest (MIT), nlohmann-json (MIT) and zlib. The
optional TLS support uses the system OpenSSL on Linux (OpenSSL 3:
Apache-2.0, compatible with GPLv3) and Windows' own SChannel. The licence
notices of what is linked into `em34xxw` are shown in its Help > About.
