# 3472 bring-up notes

Hardware facts and hypotheses for the IBM 3472 InfoWindow, a monochrome 3270
display on coax (CUT mode) with its own tube, running the ROM pair
`09f4604.bin`/`09f4605.bin` (`roms/ROMS.md`). The model is
`src/models/model_3472.cpp`. Its board is the 3483's board model
(`hw/board_3483.hpp`, docs/bringup-3483.md) at the 3472's addresses
(`CoaxBoardLayout`). Status: **bring-up**. Each entry is labelled
**observed** (in emulation or in the firmware's use of the hardware),
**documented**, or **hypothesis**.

## Summary

- POST runs its sequence (entries 1, 2, 4, 6-C), passes its video test (6)
  and stops at its setup check with `M12` on row 0, as a 3483 with an empty
  battery-backed RAM does (observed).
- Reset continues to the start-up screen: the IBM logo, "3472 Control
  Program" and "Copyright IBM Corp. 1989". The terminal then idles, waiting
  for a controller (observed; the waiting is a hypothesis).
- Against a TN3270 host (moshix.tech:2300, a 3270 BBS) the CUT controller
  connects. The host's screen and the controller's status line ("TN3270")
  show (observed, 2026-11-27).
- `tests/rc/3472-smoke.rc` checks the POST and start-up screens.
- POST's error line is only the letters, e.g. `M12`. The 3483 also shows a
  test number, an error code and data words (observed: the writer at
  `E000:3C3A` stores three cells and stops). The terminal then waits for
  Reset, as the 3483 does.

## Memory map

| Range | What | Basis |
| --- | --- | --- |
| `00000-0FFFF` | DRAM (64 KiB); regen buffer from `0:7A00` | parts observed; regen: the row lists' 3D00h cell bias (`E000:38EE`) and POST's M12 line there |
| `10000-1FFFF` | SRAM: character generator `10000-1BFFF` (companion planes `10000`/`12000`, main planes `18000`/`1A000`), setup above | observed use |
| `20000-207FF` | gate array RAM: row lists at `20108`/`20508` | observed use |
| `28000` | display controller (the 3483's `58000`) | observed: same register offsets |
| `28100` | coax adapter, interrupt status (the 3483's `58100`) | observed |
| `30000` | system control (the 3483's `58400`) | observed |
| `C0000-FFFFF` | ROM pair, interleaved | observed |

The CPU is an 8086 with no interrupt controller of its own. The board
model is the interrupt controller and supplies vector 20h (hypothesis).
The clock is 8 MHz (hypothesis). POST's timing windows pass with it.

## Differences from the 3483

- **Vertical sync** reads high during the pulse in the system sense
  register, bit 2. On the 3483 it reads low there (observed: POST test 6
  times the high phase at 14-39 loops, the low phase at 756-2265;
  `E000:2F3A`, `E000:2F5C`).
- **No colour drive table** (`28040-6F` stays zero, observed). The video
  sense outputs are the colour index bits: blue 1, red 2, green 4
  (hypothesis). That also matches what the 3483's own POST table gives
  through its threshold, and fits all of test 6's readings.
- **12-dot cells**: a glyph row is main bits 3-0 then companion bits 7-0
  (observed in the unpacked fonts, e.g. `A` spans bits 10-2). The 3483
  uses 9 dots. The test glyphs light main bit 0 and companion bit 7, two
  adjacent dots (observed).
- **Row lists** have the same layout as the 3483's (observed in the runtime
  list):
  - row 0 from entry 60 - rows (`3D00`, the regen origin);
  - rows 1 to rows - 1 explicit from entry 61 down;
  - two "continue" entries below the screen.
  The cell bias is 3D00h, so the regen origin is `0:7A00`.
- The picture is drawn in two intensities for the phosphor (hypothesis:
  high intensity brighter).
- **Status row** at `0:96F8` (observed). The status routine (`E000:A8AE`)
  builds the operator information area from coax cells 0-79, as the 3483's
  does. It then copies 84h cells to `0:96F8` (`E000:AA01`). The runtime row
  list names the same cell (`4B7C`) in its two entries below the screen.
  The 3483's list names its `0:0A00` there in the same way (observed).

## Video sense (POST test 6, `E000:2B4A`)

This is the same test as the 3483's POST test 7. It has fifteen subtests,
table at `E000:2B04`:
- one per screen model (2-5) and mode, plus a dark screen;
- expected counts 2, 1 or 0 for selectors 3, 2 and 4;
- the comparison `cmp al, ah` at `E000:2D17`.

What made it pass (hypotheses, shared with the 3483, docs/bringup-3483.md):
- the sense sees all 16 bits of all 32 rows of each glyph;
- it sees the status row below the screen, from entry 62 - rows;
- row 0 latches from entry 60 - rows.

## Open

- The coax buffer at `0:C000` and the memory engine work as on the 3483
  (observed: the engine copies `C0A0` to `7A00`, 780h words). The
  translate tables' addresses are the 3483's (untested).
- The keyboard: the 3483's 122-key keyboard on serial channel A
  (hypothesis).
- The monitor sense code: `51`, the 3483 firmware's monochrome class
  (hypothesis).
- The CPU clock.
