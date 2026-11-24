# Models, variants and phosphors

What a user selects is a **variant** of a model, for example `3477-FG` or
`3487-HC`. A variant is one hardware identity: the configuration straps the
firmware reads, the ROM set, and what the unit announces to its host.

| Model | Variant | Unit | Straps | TN5250 type | Phosphors |
| --- | --- | --- | --- | --- | --- |
| 3477 | `3477-FG` (default) | monochrome | `0C` | `IBM-3477-FG` | green, amber, paper-white |
| 3477 | `3477-FC` | colour, a stand-in | `0C` | `IBM-3477-FC` | - |
| 3486 | `3486-BA` (default) | monochrome | `42` | `IBM-5251-11` | green, amber, paper-white |
| 3487 | `3487-HG` (default) | monochrome | `4A` | `IBM-5251-11` | green, amber, paper-white |
| 3487 | `3487-HC` | colour | `4C` | `IBM-3179-2` | - |

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
- `3477-FC`: a **stand-in**. This ROM cannot identify as colour (observed,
  docs/bringup-3477.md), so the unit runs as the monochrome one, and only
  the host (terminal type `IBM-3477-FC`) and the picture treat it as
  colour. It is to be replaced by the colour 3477's own ROM.
- `3486-BA`: the firmware reports `BA0` for these straps (observed).
- `3487-HG`, `3487-HC`: the firmware's `HG0`/`HA0` and `HC0` (observed).

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
