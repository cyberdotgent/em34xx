# tn34xx

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

```sh
export VCPKG_ROOT=~/vcpkg
cmake --preset release
cmake --build --preset release
```

The language level is deliberately C++17: the project must stay buildable
with toolchains that target Windows 7 (x86).

## ROMs

ROM images are not distributed. Put them in `roms/` under the names listed in
[roms/ROMS.md](roms/ROMS.md); they are git-ignored and verified by SHA-256.
Known-defective dumps are refused unless `--allow-bad-rom` is given.

```sh
build/release/tn34xx --check-roms --model 3477
```

The emulator is a clean implementation: it contains no code or data taken
from the ROMs, and it never patches them. Hardware behaviour is emulated
instead.

## Running

```sh
build/release/tn34xx --model 3477              # window + monitor on the terminal
build/release/tn34xx --headless -S             # monitor only, CPU stopped at reset
build/release/tn34xx --batch --rc script.rc    # automated run; exit code from the script
build/release/tn34xx --gdb -S                  # GDB stub on 127.0.0.1:1234
```

One ROM serves every display subtype and language of a model. Choose the
tube with `--display green|amber|paperwhite|color` (or the shorthands
`--green`, `--amber`, `--paperwhite`, `--color`). The keyboard decides the
terminal's language through its ID, so it is configured too:
`--keyboard 122|enhanced --keyboard-layout us` (see
[docs/keyboard.md](docs/keyboard.md)).

Configuration can also come from a TOML file (`--config`); see
[tn34xx.example.toml](tn34xx.example.toml). `--dump-config` prints the
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
gdb-multiarch -x tools/tn34xx.gdb -ex 'target remote :1234'
```

The stub describes an i8086 target. `eip` is the linear address of CS:IP,
and memory addresses are linear. `monitor <cmd>` runs any monitor command;
its output streams to GDB while the command runs.

[tools/tn34xx.gdb](tools/tn34xx.gdb) sets up 16-bit Intel-syntax
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

Not yet decided. It will be a free-software licence chosen to be compatible
with the dependencies (all currently permissive: SDL3 zlib, replxx BSD,
toml++ MIT, doctest MIT, nlohmann-json MIT, zlib).
