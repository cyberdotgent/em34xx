# Models, variants and phosphors

What a user selects is a **variant** of a model, for example `3477-FG` or
`3487-HC`. A variant is one hardware identity: the configuration straps the
firmware reads, the ROM set, and what the unit announces to its host.

| Model | Variant | Unit | Straps | TN5250 type | Phosphors |
| --- | --- | --- | --- | --- | --- |
| 3477 | `3477-FG` (default) | monochrome | `0C` | `IBM-3477-FG` | green, amber, paper-white |
| 3477 | `3477-FC` | colour, its own ROM pair | `02` | `IBM-3477-FC` | - |
| 3486 | `3486-BA` (default) | monochrome | `42` | `IBM-5251-11` | green, amber, paper-white |
| 3487 | `3487-HG` (default) | monochrome | `4A` | `IBM-5251-11` | green, amber, paper-white |
| 3487 | `3487-HC` | colour | `4C` | `IBM-3179-2` | - |
| 3472 | `3472-G` | 3270 display on coax, monochrome (bring-up skeleton) | - | - (TN3270) | green, amber, paper-white |
| 3483 | `3483-V` | 3270 display on coax, colour VGA monitor (bring-up) | sense `44` | - (TN3270) | - |

`em34xx --list-models` prints the same from the code (`ModelInfo::variants`
in `src/machine/model_info.hpp`, filled in by each model).

## One monochrome unit, whatever the tube

A monochrome variant is **one** unit. Its tube (the **phosphor**: green,
amber, paper-white) changes only the picture: it is a setting of the
presentation, can be changed while the terminal runs, and is never visible
to the firmware or the host. The unit always announces its first (green)
type, so a host never sends colour to a monochrome unit or the reverse.

- Decision (project owner): three terminal types for one board with another
  tube fitted are not worth having.
- Observed on real units (project owner): an amber tube in a green unit
  works; the difference is the phosphor and the label on the box.
- What this overrides (observed in the firmware): both families have a
  paper-white display type of their own, the 3477 with straps `0E` (type 5,
  VPD suffix `FW0`, its own cell heights) and the 3487 with straps `48`
  (type 5, `HW0`, its own video timing). No variant selects them. They stay
  reachable with `--straps 0E` / `--straps 48`.

A variant lists the phosphors it offers (`ModelVariant::phosphors`, each
with an id, a label and a colour). Front ends and configuration files take
the list from the model, so a tube added to a variant appears everywhere
without further changes.

## Names

- `3477-FG`: FG is the green 3477 (documented: the TN5250 type
  `IBM-3477-FG`). The firmware's own VPD suffix for the straps used is
  `FE0` (observed); which real tube `FE` is has not been established.
- `3477-FC`: the colour 3477, which runs its own firmware
  (`65f2995.bin`, `65f2996.bin`) and reports `FC0`, model ID `50`
  (observed, docs/bringup-3477.md).
- `3486-BA`: the firmware reports `BA0` for these straps (observed).
- `3487-HG`, `3487-HC`: the firmware's `HG0`/`HA0` and `HC0` (observed).

## A variant can have its own firmware

Variants of a model normally share its ROM set (`ModelInfo::roms`). One
that runs other firmware names its own (`ModelVariant::roms`); the 3477 is
the case: each of its two ROM pairs accepts only its own monitor, so
`3477-FG` and `3477-FC` are different images on the same board.

- ROM checks, `--check-roms`, the ROM dialogs and the terminal type lists
  work per variant: with only one pair present, the other variant is shown
  as needing its ROM images, and a bare `--model 3477` in a front end
  starts the variant that can run.
- Saved state follows the firmware (`Model::storage_name`): variants that
  share the model's ROM set share `<model>.nvram` and `<model>.eeprom`;
  one with its own firmware has `<variant>.nvram` (`3477-FC.nvram`),
  because each firmware keeps the setup in its own format (observed: the
  colour 3477 resets a setup saved by the monochrome one).
- Where the board behaves differently for such a variant, the model picks
  another description of the same part (`kDisplayLayout3477Colour` in
  `src/models/board_3477.hpp`); it is still one model class.

## Selecting

- Command line: `--model 3487-HC`; a bare `--model 3487` is the model's
  first variant. `--phosphor amber` selects the tube.
- `--display green|amber|paperwhite|color` and `--green` etc. of older
  versions still work: a monochrome name is that phosphor on the model's
  monochrome variant, `color` is its colour variant (an error when the model
  has none, as the 3486).
- Profiles (`.em34xx`, format 3): `model = "3477-FG"` and
  `[display] phosphor = "amber"`. Formats 1 and 2 (`model = "3477"`,
  `[display] subtype`) are read the same way as `--display`; colour on a
  model without a colour variant becomes its first variant.
- Changing the phosphor never restarts the terminal
  (`Application::set_phosphor()`); changing the variant is changing the unit
  (`Application::set_model()`): the machine is rebuilt and powered on and
  the sessions reconnect with the new terminal type.

## CPU

The CPU is a property of the model (`ModelInfo::cpu`) and the core behaves
as that chip (`I86::chip_for()` in `src/cpu/i86.cpp`: instruction set,
edge cases and the timing model's figures per chip).

| Model | CPU | Core |
| --- | --- | --- |
| 3477 | AMD N8088-2 (documented) | 8088 |
| 3486, 3487 | NEC V20, D70108L-8 (observed) | V20 |
| 3489 | 80286 | not implemented (no protected-mode core; the model is a placeholder) |

An 8086 would behave as the 8088 does here apart from its bus width. The
8088 core is checked against the SingleStepTests 8088 vectors
(`tools/fetch-test-vectors.sh`); that project also publishes V20 and 80286
sets, which are not wired in yet.

## Options

A model declares its own switches (`ModelInfo::options`): today the
temporary stand-ins that can be turned off (`vdo-shim` on both families,
`alignment-shim` on the 3486 family). `--list-models` shows them. They are
set with `--set NAME=on|off` or `[emulator.options]` in the configuration
file; `--no-vdo-shim`, `--no-alignment-shim` and the `[emulator]` keys
`vdo_shim` / `alignment_shim` remain as shorthands. A switch the running
model does not declare is ignored with a warning.

## How a model is put together

- `src/hw/`: the hardware the terminal families share, under neutral names
  (`DisplayController`, `SystemGateArray`, `KeyboardInterface`,
  `TwinaxEngine`, `I2cEeprom`, `BatteryBackedRam`, `AuxPort`). Where boards
  differ, the part takes a description with **no defaults**
  (`DisplayLayout`, `TwinaxEngineLayout`, the number of link channels), so
  a model cannot inherit another board's value by leaving one out.
- `src/models/board_*.hpp`: one board's descriptions and straps.
- `src/models/parts.*`: the parts a terminal is built from; each call
  creates the hardware, maps it and connects it.
- `src/models/model_*.cpp`: one class per terminal. It states the facts
  (`ModelInfo`: CPU, clock, line, sessions, variants, options, ROMs, notes)
  and its `build()` is the list of parts. The 3486 and 3487 are two classes
  over `InfoWindow2Board`, which holds what they share.

Adding a terminal on an existing board is a new class with its variants.
A new board is a `board_*.hpp`, plus new parts only for hardware the others
do not have.

## Line and controller

A model states the line it hangs on (`ModelInfo::line`,
`machine/line_port.hpp`): `Twinax` for every model so far. The machine holds
a `LineHost` and only accepts one for its own line. Each kind of line has
its own host interface on top of that: `TwinaxHost` exchanges Twinax frames
with the link engine, and its controllers are in `src/host/twinax/` (the
workstation controller, and the bridge that puts TN5250 sessions behind
it).

`Coax` exists as a line kind with no host interface and no controller yet.
A coax terminal would add a `CoaxHost`, the line side of its engine in
`src/hw`, and a controller under `src/host/coax/` that wraps TN3270 the way
the workstation controller wraps TN5250. Nothing about coax hardware is
assumed in the code: the Twinax engine has not been split into a shared
core and a line front end, because what the two would share is not known
until a coax terminal's firmware has been looked at.
