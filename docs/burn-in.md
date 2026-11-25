# Screen burn-in

A "fun" feature (CONTRIBUTING.md): the tube wears where it is lit, as a
real one does, only much sooner. It belongs to the presentation
(`src/present/burn_in.*`); the emulated machine never sees it, and it is
off in the monitor and in tests unless asked for.

Nothing here is taken from a terminal: the model and its numbers are ours.

## What you see

- Where the phosphor has been lit a lot it gives less light: new text over
  an old picture is dimmer there.
- The unlit glass is dark grey instead of black, and darker and browner
  where it is worn, so the old picture shows as a ghost on a dark screen.
  This grey is there with a new tube too, whenever burn-in is not Off.
- Auto Dim (docs/bringup-3477.md) protects the tube, as it was meant to.

## Settings

Application-wide, not per profile: there is one tube.

| Where | What |
| --- | --- |
| View > Burn-in > Off / Slow / Fast / Hyper | the speed; Off neither wears nor shows the tube, and keeps its wear |
| View > Burn-in > Replace CRT... | a new tube, after a question |
| Settings | the same speed |
| `settings.toml` | `[display]` `burn_in = "off"`, `"slow"` (default), `"fast"` or `"hyper"` |
| monitor | `burnin [off\|slow\|fast\|hyper\|expose SECONDS\|reset]`: the monitor's own tube, off at start and never saved; it shows in `screenshot` |

Time for the same picture, lit at full brightness, to leave a mark:

| Speed | Just visible (2% of the light lost) | Obvious (20%) |
| --- | --- | --- |
| Slow | 16 hours | 230 hours |
| Fast | 2 hours | 29 hours |
| Hyper | 10 seconds | 2.5 minutes |

Hyper is for testing, and for playing. A real tube takes thousands of
hours. Wear from any speed is kept; Replace CRT is the way back.

## How it works

- **Wear.** A map of 1024 x 768 values covers the whole glass, border
  included. Twice a second the front end adds the picture on the tube to
  it: mean brightness of each spot (the brightest gun), times the seconds
  since the last tick (2 at most, so sleep does not count; time with a
  dialog open does, the tube is lit),
  times the speed. Wear only grows.
- **Depth.** `depth = 1 - exp(-wear / 271)`, in hours of full brightness at
  Slow. A lit spot gives `1 - 0.35 * depth` of its light: a worn-out tube
  still gives 65%.
- **One tube for every model.** The map is in glass coordinates, not
  raster pixels, so a ghost stays where it is when the model or the screen
  format changes. One layer of phosphor: a colour tube wears like a
  monochrome one.
- **Drawing.** `CrtRenderer` applies the map last, after the beam's bloom
  and clipping. The picture is drawn again from the map when some spot's
  depth has moved by 0.4%: about hourly at Slow, every tick at Hyper.
- **The file.** `tube.burn` in the user folder: `EM34TUBE`, version, map
  size, hours of use, then the map as 32-bit floats (3 MB). Written every
  five minutes when changed, on exit and at log-off, to a temporary file
  that replaces the old one. A file of another version or size is ignored
  and the tube starts new.
- **Several windows** (em34xx.app) share the tube and each shows it. The
  tube ages with the time the application runs, not with the number of
  windows: each window's picture counts for its share of the time.

## Status

- em34xxw: used under wine (menu, Settings, the ghost after a reboot,
  the file across a restart, Replace CRT).
- em34xx.app: written without a Mac; compiled by CI, not yet run by hand.
- The SDL developer window does not have it; the monitor's `burnin` does.
