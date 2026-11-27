# Monitor reference

Commands come from the interactive prompt, from `--rc FILE` scripts, from
stdin (when it is not a terminal), and from GDB's `monitor` command.

## Scripted runs

A script or a pipe drives the machine deterministically: `run n` executes
`n` instructions (there is no wall-clock wait), `key` presses host keys,
`screenshot` saves the picture. With `--rc FILE` the machine stays stopped
until the script runs it; commands piped on stdin need `-S` (`--stopped`),
otherwise the machine is already running and `run` fails. On the 3477,
`run #40000000` reaches the setup screen; Right Ctrl (`key rctrl`) is the
terminal's Enter on the default keymap.

```sh
printf 'speed max\nrun #40000000\nkey rctrl\nrun #6000000\nscreenshot s.bmp\nquit\n' |
  em34xx-headless -S --model 3477 --rom-dir roms
em34xx-headless --model 3477 --rom-dir roms --rc boot.rc --batch
```

`em34xx` (the SDL window) reads its console only from a terminal; give it
`--rc FILE` or use GDB's `monitor` when stdin is not one. A `quit` in the
rc file exits before the window opens.

## Syntax

- Numbers are hexadecimal: `1f00`, `0x1f00`, `1f00h`. Use `#123` for decimal.
- Addresses are linear (`f0054`) or `segment:offset` (`f000:54`, `ds:si`).
- Expressions support registers (`ax`…`di`, `al`…`bh`, `es cs ss ds`, `ip`,
  `flags`, `pc` = linear CS:IP), memory (`byte[addr]`, `word[addr]`) and the
  operators `+ - * / % & | ^ ~ << >> == != < <= > >= && || !`.
- An expression containing spaces must be quoted: `assert "ax == 1234"`.
- `#` or `;` at the start of a line begins a comment.

## Execution

| Command | Description |
| --- | --- |
| `go [addr]` (`g`, `c`) | Run until a stop condition. Ctrl-C stops. With `addr`, adds a temporary breakpoint. |
| `bg [addr]` | Run in the background; the prompt stays available. |
| `stop` | Stop a background run. |
| `run n` | Run `n` instructions. |
| `step [n]` (`s`) | Execute `n` instructions and show the state. |
| `trace [n]` (`t`) | Execute `n` instructions, printing each one. |
| `proceed` (`p`) | Step over a CALL/INT. |
| `reset` | Reset the machine. |
| `speed [pct\|max]` | Pace to a percentage of the nominal clock; `max` runs unthrottled. |

## Breakpoints

| Command | Description |
| --- | --- |
| `bp addr [do "cmds"]` | Execution breakpoint. With `do`, a hit runs `cmds` (separated by `;`) and the run goes on. |
| `wp addr [len] [r\|w\|rw] [do "cmds"]` | Watchpoint (default: write); `do` as for `bp`. |
| `bl` | List breakpoints and watchpoints. |
| `bc id\|addr\|*`, `wc id\|*` | Clear breakpoints or watchpoints. |

### Logging breakpoints

A breakpoint or watchpoint with `do` logs instead of stopping: on a hit the
monitor prints one line (the breakpoint's location, or the watchpoint's
access), runs the commands and continues, within the budget of the `run`
or `go` that is running. So one `run` logs every call of a routine or every
write to a range, with no stop-and-resume loop:

```
bp d000:7a3e do "print word[ss:sp+6] word[ss:sp+4] word[ss:sp+a] word[ss:sp+8]"
wp 40000 8000 w do "print word[58020] cx di"
run #300000000
```

Actions cannot run the machine (`go`, `run`, `step`, ...); `stop` in an
action ends the run at that hit. A `go` with logging breakpoints only
returns at another stop, so give such scripts a `run n` instead.
Watchpoints see the CPU's accesses at the address it uses: a device that
mirrors a write elsewhere (the 3483's 40000 window, for one) does not
trigger a watchpoint on the mirror.

## Memory and state

| Command | Description |
| --- | --- |
| `regs [reg=value ...]` (`r`) | Show or assign registers. |
| `dump [addr] [len]` (`d`) | Hex dump; continues from the last dump. |
| `unasm [addr] [count]` (`u`) | Disassemble; defaults to CS:IP. |
| `enter addr bytes... \| "text"` (`e`) | Write RAM. |
| `fill addr len byte` (`f`) | Fill RAM. |
| `savemem file addr len` | Save memory to a file. |

## Hardware discovery

| Command | Description |
| --- | --- |
| `map` | Show the memory map and policy. |
| `map ram addr len` | Add RAM. |
| `map probe addr len [latch\|value]` | Add a logging stand-in device. A latch behaves like RAM; a value returns that byte on every read. |
| `unmap addr len` | Remove mappings overlapping a range. |
| `io`, `io probe port count [latch\|value]` | Same for I/O ports. |
| `dev [name]` | List devices, or show one device's state (probes show per-offset counts). |
| `unmapped [clear]` | Every unexpected access so far: address, direction, count, first PC, last value. |
| `policy stop\|log\|ignore` | Unexpected-access policy. `stop` halts the first time each address/direction is touched. |
| `log spec` | Log levels, e.g. `bus=debug,mmio=trace,io=trace`. |

## Host link

| Command | Description |
| --- | --- |
| `twinax status` | Show the controller-side state: per station the cable (plugged/unplugged), state and counters; with the TN5250 bridge also each session's host, TLS, terminal type, device name and whether the terminal is on line. `info` shows the same. |
| `connect host[:port] [tls] [station n] [device name] [type t] [option value ...]` | Attach the TN5250 bridge (docs/twinax-bridge.md) to the Twinax line at run time. Port 23, or 992 with `tls`. Settings not given come from the configuration file and command line. `station n` uses only display station `n`; `device` and `type` set DEVNAME and the terminal type; `port`, `codepage`, `ca`, `env NAME=VALUE` and any other `name value` (or `name=value`) become `--tn5250-name value`, so every TLS option works (`tls-verify none`, `tls-pin HASH`, `tls-min 1.0`, ...). The controller is plugged into the cable once the TN5250 session is negotiated. `connect` while connected closes the current sessions and starts again from scratch. |
| `disconnect [station]` | Without a station: close every session and take the controller off the line (no controller, as with a pulled cable) until the next `connect` or `reset`. With a station: close that station's session and keep its cable unplugged until the terminal is reset. |
| `twinax send dd/a ...` | Queue a message of frames (`data/address`, hex/decimal). Needs `--twinax-host script`. |
| `twinax recv` | Show messages the terminal sent. |

## Scripting

| Command | Description |
| --- | --- |
| `assert expr [message]` | Count a failure if `expr` is zero. |
| `echo text` | Print text. |
| `print expr ...` | Print expressions in hex, e.g. `print ax word[ss:sp+4] byte[2000:6fc]`. |
| `source file` | Run another script. |
| `screenshot file.bmp\|file.ppm [raw]` | Save the display, stretched to the tube's proportions (4:3 on the 3477) unless `raw`. |
| `burnin [off\|slow\|fast\|hyper\|expose SECONDS\|reset]` | Screen burn-in of the monitor's own tube (docs/burn-in.md): set the speed, leave the current picture on the tube for that long, or fit a new tube. Off at start and never saved; it shows in `screenshot`. |
| `quit [code]` | Exit. The default code is 1 if any assertion failed, otherwise 0. |

A script stops at the first command error. With `--batch`, a failed script
exits with code 2.

## From GDB

`monitor <cmd>` in GDB runs a command on the stub's connection. Output is
sent as GDB console-output packets (at most 1 KiB of text each) while the
command runs, so long output such as `monitor trace #500` arrives complete.
The reply is always `OK`; a failed command shows `error: ...` in its output.
`tools/em34xx.gdb` runs `monitor regs` at every stop. GDB addresses are
linear: use `lin`, `bseg`, `useg` and `wseg` from that file for `seg:off`.
