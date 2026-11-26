# 3483 bring-up notes

Hardware facts and hypotheses for the IBM 3483 InfoWindow II, a 3270
display on coax (CUT mode) with a VGA monitor, running the firmware
`06g5525.bin` (`roms/ROMS.md`). The model is `src/models/model_3483.cpp`,
the board devices `src/hw/board_3483.{hpp,cpp}`; its status is
**bring-up**. Each entry is labelled **observed** (seen in emulation, in
the firmware's use of the hardware, or in photos of a real 3483 taken by
the project owner), **documented** (with the source), or **hypothesis**.
No board documentation is available: everything here comes from the
firmware's own use of the registers. This file describes hardware only;
the coax line and the controller side are documented with them.

## Summary

- POST passes, the first boot with an empty battery-backed RAM stops at its
  NVR line, Reset continues and the firmware rebuilds its setup (observed).
- The runtime shows Offline Setup, Customization, the Printer Definition
  Utility, Serial Number and the display test's cursor page in colour; the
  keyboard works (observed).
- A setup changed in Customization is saved in the battery-backed RAM and
  is in force after a reboot with the same `--nvram` (observed). The next
  boot still opens Offline Setup (open question below).
- The display follows the mode registers (24x80, 32x80, 43x80, 27x132),
  draws the hardware cursor, the rule line, blink, reverse and both
  underscore styles, and Black-On-White emulation (observed register use;
  the drawing details are hypotheses, see below).
- The alarm and the keyboard clicker reach the machine's audio (observed).

## CPU and memory map

| Range | What | Basis |
| --- | --- | --- |
| `00000-3FFFF` | DRAM (two 256Kx4 parts). `0:0800` status row, `0:1000` regen buffer | parts observed on the board; placement hypothesis |
| `40000-4FFFF` | battery-backed SRAM (two 32Kx8 parts): character sets, setup, POST history | parts observed; placement hypothesis |
| `50000-507FF` | block RAM: display row lists at `50108`/`50508`, coax work area `50308` | observed use |
| `58000` | display controller | observed use |
| `58100` | coax adapter, memory engine, interrupt status | observed use |
| `58400` | system control: alarm, serial channels, sense | observed use |
| `80000-FFFFF` | ROM `06G5525`, reset vector `C000:8DFC` | observed |

The CPU is an IBM ASIC with an 80186 core (39G8873, observed: marking, and
the firmware relocates the 80186 control block to `8FF00`). Its chip
selects, timers and DMA are never programmed (observed): the board decodes
memory itself. All interrupts come in on INT0 in cascade mode; the
firmware installs handlers at vectors 20h and 21h only, and the model
supplies 20h (hypothesis). The clock (10 MHz) is a hypothesis; the
oscillator is not legible in the board photo.

## POST

Reset writes the display, coax and system registers to known values,
`58402` = `809B` (the alarm on, see below), tests low RAM and the ROM
segment sums (`1234`/`5678`/`9ABC`/`DEF0` per 64 KiB, observed), writes
`8000` to `58402` and runs its test sequencer. The normal (mode 0) order is
tests 3, 4, 5, 7, 11-17, 9 and 10 (observed, descriptor table
`C000:8C2A`). All of them pass in emulation (observed).

A failing test draws a line at the top of the screen (`C000:96C8`,
observed): a set of letters, the test number, three error-code characters
and six words. The letters come from the string `LMDKC012`: each appears
when its bit of the test's FRU byte (`[04CB]`, bit 7 for `L` down to bit 0
for `2`) is set (observed). Most tests carry `80` (`L`); test 11 carries
`08` (`C`), test 14 `90` (`LK`), test 15 `C0` (`LM`), test 16 `10` (`K`),
tests 10 and 17 `40` (`M`) (observed). Test 10, the setup check, adds bit 1
(`1`) when the setup blocks are invalid and bit 0 (`2`) when the check byte
at `47E2F` is wrong (observed, `C000:BAF8`). So **`M12`** is test 10 on a
unit whose battery-backed RAM is empty or lost: both setup copies and the
check byte bad (observed in emulation with an empty `--nvram`). What the
letters stand for is a hypothesis: `L` the logic unit, `K` the keyboard,
`C` the coax adapter or an option card; `M` is shared by the setup check,
the keyboard handshake and channel B, so its meaning is open.

## Display controller (`58000`)

### Mode records

The firmware sets a display mode by copying a 38-byte record from ROM
`81A0C` (12 records, chosen through the table at `D000:59FE` by display
type and size) into the controller (observed, `D000:564C`):

| Record word | Register | Use |
| --- | --- | --- |
| 0-7 | `58000-5800E` | timing pairs, 6845-like |
| 11 | `58016` | cursor lines (bit 7 from the setup) |
| 12 | `58018` | `0095` in every record (unknown) |
| 13 | `5801A` shadow | control |
| 14 (high byte) | `5801C` high byte | control |
| 16 | `58026` | status and control |
| 17 | `58028` | differs per monitor class (unknown) |
| 18 | `58400` | `0080` (unknown) |

Geometry (observed record contents, hypothesis for the meaning, all fitted
to every record):

- `58000` low byte = columns - 1: `4F` (80) or `83` (132).
- `58006` low byte = rows - 1: `17`, `1F`, `2A`, `1A` for 24, 32, 43 and
  27 rows.
- `5800A` low byte = scan lines per cell: `12`, `0D`, `09`, `10` (18, 13,
  9 and 16 lines). POST's blank test screen holds glyph 02, whose only dots
  are in its bytes 18-19, and expects no light with `12` here (observed),
  so an 18-line cell shows glyph bytes 0-17.
- `58016`: the underline cursor's first (low byte, bits 4-0) and last (high
  byte) scan line, counted from 1: `1211`, `0D0C`, `0908`, `100F` are the
  last two lines of each cell, `0909` and `0B0B` the last line (observed
  values; the numbering from 1 is the only reading that keeps every record
  inside its cell, hypothesis).

Customization's "Screen rows x columns" switches between 24x80, 32x80 and
43x80 on the 3483-V (observed in emulation and on the real unit's
Customization photo; 27x132 is not offered there). The setup screens
themselves always run in 24x80 (observed). In emulation, choosing 32x80
and leaving setup gives 33 rows of 13-line cells (observed).

### Screen rows and the status row

`5801A` bit 6 set: a linear screen from `0:1000` (POST's screens, observed).
Clear: each row's start comes from the row list in block RAM, entry r at
`50108+4r` (A) and `50508+4r` (B), copied there by `svc_244` from the
firmware's data segment (observed). The runtime sets A bit 7 on every row
and its screens are linear at `0:1000` (observed), so A bit 7 is taken as
"continue after the previous row" (hypothesis). Otherwise the row starts at
cell `((A & 7F) << 8) | B[0]`, biased by 800h cells (hypothesis fitted to
the display test). The other B bytes (`01`, `7C` or `B0`) and A's low bits
are not understood (open).

The status row (the 3270 operator information area) is the 80 cells at
`0:0800`, shown below the screen with a thin rule along its top edge
(observed: the runtime keeps "SETUP" in reverse there, and photos of a real
3483 show that bar under a blue line on every setup screen). No register
that points to it has been found; a fixed address is a hypothesis.

`5801C` is **not** a start address (it was first taken for one): the
runtime keeps control bits in it (observed, shadow `[026E]`):

| Bit | Meaning | Basis |
| --- | --- | --- |
| 7 | cursor shown | observed: set whenever the cursor is placed (`D000:2F40`), cleared to hide it |
| 6 | block (reverse) cursor | observed: setup item 23h, the keyboard's cursor toggle and the controller's control-register bit 1 flip it |
| 5 | blinking cursor | observed: setup item 24h, the keyboard's blink toggle and control-register bit 0 |
| 3 | with `58014` (see below) | observed |
| 2 | set by the setup's display init | observed, meaning open |
| 12 | from the mode record (`1000`); cleared with linear mode by the display test | observed, meaning open |
| 13 | cleared for the monochrome monitor class and for Black-On-White emulation, also clear in the display test (`1080`) | observed, meaning open |

The coax adapter's control register (`5030A`, loaded by the controller's
LOAD CONTROL REGISTER) reaches bits 5 and 6 through the firmware: its bit 0
(cursor blink) toggles `[026E]` bit 5 and its bit 1 (reverse/block cursor)
bit 6 (observed, handler `F000:9D67`, per the coax adapter analysis), which
supports the block/blink assignment above.

This explains the values seen: `1000` is the mode record's default during
POST, `0` the display test's reset, `3004` and `1004` the runtime with and
without bit 13. Bit 13 is not modelled.

### Cursor

- `58010` = row (high byte) and column (low byte) on the screen (observed:
  the cursor routine `D000:2F40` divides the buffer offset by the line
  length and writes row:column, with offsets for a mirrored screen; the
  display test places it at row 13, column 9).
- Shown by `5801C` bit 7; block or underline by bit 6; blink by bit 5
  (above). The underline cursor lights the `58016` lines; the block cursor
  reverses the cell (hypothesis, photos 1220-1222 of the real unit show
  both styles over a full cell).
- Blink phase: about 1 Hz (hypothesis), shared with blinking characters.
- The setup screens never turn the hardware cursor on: their "cursor" is a
  reverse-video bar written into the regen buffer (observed). The display
  test's first page is the cursor alone at 13,9 (observed in emulation).

`58014` takes the cursor column in its low byte and a row in its high byte
(the last screen row, the status row, or a value the host sends) and is
enabled with `5801C` bit 3 (observed, `svc_174`); `FFFF` otherwise. Its
use is open (hypothesis: the cursor shown in the operator information
area). `58022`/`58024` are written `FFFF` beside it (open).

### Rule line

`58012` = rule row (high byte) and rule column (low byte), `FF` for none;
`5801E` bit 6 draws the horizontal rule, bit 5 the vertical one (observed:
`D000:3632` maps Display Setup's "Horizontal", "Vertical", "Cross" and
"None" to bits 6, 5, both and neither). When "Follows cursor" is on, the
cursor routine copies the cursor position into `58012` (observed). The
model draws a dotted horizontal rule on the cell's last line and a
vertical rule as the first dot column of the cell, in white (hypothesis;
photos 1363-1372 show a dotted horizontal rule). Rule colour (white or
green) is not mapped yet. `5801E` bit 3 is the mirrored (right-to-left)
screen (observed use, not modelled).

### Cells and highlighting

A cell is two bytes: a character in 3270 buffer code and a 3270 extended
attribute (EAB). Characters `C0-FF` are field attributes (observed:
Offline Setup's hidden values sit in `FC` fields). EAB bits 7-6 are the
highlight (01 blink, 10 reverse, 11 underscore), bits 5-3 the colour, bits
2-0 the character set (1 = APL/GE). A cell without a colour, highlight or
character set of its own takes its field's (observed: Customization's
underscore-style samples are GE line characters with no EAB of their own;
taking the base set would show them as "Z", the real unit shows lines).

- Reverse: the cell in its colour, the glyph dark (observed in photos: the
  selected item bar is pale yellow with black text). The real unit has a
  separate, paler reverse-video palette (Color Palette Setup page 2,
  observed); the model keeps the shared terminal colours (project owner's
  decision).
- Underscore: on the underline cursor's lines (hypothesis: the serial
  number photo puts the underline at 0.875-0.97 of the cell). `58016` bit 7
  set draws it solid, clear dashed (observed: Customization's "Underscore
  style" changes that bit alone; the dash pattern, a gap at each end of the
  cell, is a hypothesis).
- Blink: the character is dark in every other phase (hypothesis for the
  rate).
- Black-On-White emulation sets `5801A` bits 15 and 1, clears bit 0 and
  `5801C` bit 13, and changes `58004` (`634F` to `6151`) and `58026` bit 9
  (observed). The model draws the picture reversed, black on white, for
  `5801A` bit 15 (hypothesis).
- "Unprotected field markup" sets `58026` bit 6 (observed); the markup
  character (dots, underline or another glyph, Customization's next line)
  is not modelled.

The colour table at `58040-6F` (one block per gun) only feeds POST's video
sense; the picture uses the shared terminal colours
(`hw/terminal_colours.hpp`).

## System control (`58400`)

### Alarm and clicker (`58402`)

- Bit 7 sounds the alarm, bits 6-0 a level (observed: `svc_177` sets bit 7
  with bits 6-0 = setup byte `3C7`, Display Setup's alarm volume, item 28h,
  for 8 ticks, then clears it; POST's beep routine `C000:83CD` sets bit 7
  with a pattern value).
- Bit 10 is pulsed for a keystroke click with bits 6-0 = setup byte `3C8`,
  the clicker volume, item 29h (observed).
- Reset writes `809B`: the alarm sounds from power-on until the early RAM
  and ROM checks write `8000` (observed); on a RAM or ROM failure the CPU
  halts with it still on.
- Bit 15 is always set after the checks (hypothesis: speaker enable).
- Bits 6-0 as a volume is a hypothesis; no pitch is written anywhere, so
  the model uses the sink's default pitch.

In emulation: one short tone at power-on, then one click per keystroke
(observed).

### Serial channels A and B

Channel A (`58406` control, `58408` data, interrupt `5810C` bit 4) and
channel B (`58410`, `58412`, interrupt `58120` bit 15) are the same
11-bit, odd-parity PS/2-style ports (observed). The firmware keeps a
device type per channel (`[04A6]` for A, `[04A7]` for B; 1 = keyboard,
2 = pointing device, 3 = none; POST starts with A = keyboard, B = none) and
dispatches received bytes on it (observed: `F000:9A91`, `F000:9C83`). A
type-2 device is sent `F3 28 E8 03 F4` (observed): set sample rate 40,
set resolution 3, enable, the PS/2 mouse initialisation (hypothesis,
strong). So channel B is the port for a mouse (or a keyboard on either
port). Both channels' POST tests (12-15) drive the lines themselves and
pass without a device. Nothing is attached to channel B in the model;
a mouse is future work.

### Other registers

- `5840A`: POST progress (55 at reset, the test number, AA) (observed).
- `5840C`: the runtime pulses bits 0-1 and reads a 4-bit code twice from
  bits 0-3, mapping it through a table (observed, `E000:D2F2`, `E000:D3EF`);
  not modelled (reads 0). Hypothesis: an identification or option port.
- `5840E`: POST mode (bits 1-0 = 11), vertical sync (bit 2), the monitor
  sense lines (bits 8-11, driven low by bits 12-15) (observed).

### Printer

The board has a 25-pin connector (J17) and the firmware has a Printer
Definition Utility (printer types None, IBM families, ThinkJet, DeskJet,
FX1050, LQ1050) (observed). No printer register has been found: the
firmware's far pointers name only the devices above, it uses no I/O ports
outside the tester mode, and a run through POST, setup and the online
state makes no unmapped access besides the optional-code probe at `60000`
(observed). On the coax side the printer is a second device address
(command bytes C0-DF, the adapter's second address counter; see the coax
adapter specification in the analysis notes), so print data arrives
through the coax adapter. How the firmware then drives the physical port
is open; nothing needs to be modelled for the firmware to run.

## Battery-backed RAM and setup

- Character sets: decompressed into SRAM by the firmware, 32 bytes per
  character: base set `48000`, APL/GE set `4A000`, companion planes
  `40000`/`42000` holding dots 8 and 9 (observed layout; the dot order is a
  hypothesis fitted to a GE line glyph).
- Setup: two copies of a 256-byte block at `47400` and `47500`, each with a
  check word at its end (observed: Customization changes both copies the
  same way). Byte `0A` is the screen size (2 = 24x80, 3 = 32x80; observed).
  Setup item n is kept at `[039F + n]` in the runtime's data segment
  (observed, `F000:57CE`); that this is a copy of the block from `[03A0]`
  on is a hypothesis, supported by the underscore style: `[03AD]` bit 7
  goes to `58016` bit 7, and choosing the dashed style clears bit 7 of
  byte `0D` in both copies (observed). Every save also changed bytes `98`
  (00 to 01) and `B6` (00 to 3F) (observed, meaning open).
- `47C00`: POST error history; `47C04` counts boots (observed: it goes up
  by one per boot).
- `47E26-47E2F`: a block with a check byte (`47E2F`, the sum must give
  `ED`), rewritten when wrong (observed).

Saving: change an item in Customization (Space on a choice), leave with F3
to Offline Setup and F3 again. The setup takes effect at once (32x80 above)
and is written to both copies (observed). After a reboot with the same
`--nvram`, Customization shows the saved choice as current (observed).

## Display test

"Test display station" in Offline Setup (state `[06FC]` = 92h) builds its
pattern in the coax buffer (`0:C000`) and has the memory engine copy it to
the regen buffer: engine commands `C1` (copy) and `F1` (search) (observed).
`58110` bits 5-3 are the operation and bit 6 is a flag. The runtime sets
bit 6 in state 92h and on request (F000:BBA3), and reads the results the
same either way. So it is modelled as leaving the data unchanged
(hypothesis: timing). Until this was fixed, the test showed only the
cursor at row 13, column 9, which looked like a hang.

The page shows (observed in emulation):
- the model (3483);
- the ROM part number (`06G5525`);
- digits and letters;
- colour bars (blue, blinking red, reverse pink, green underscored,
  turquoise, yellow, white);
- line samples.

The test loads its own glyphs into A0-AB (A-L) of the base set by
decompressing patches with the boot-time decompressor (svc_049). They are a
cell-edge glyph, stripes every third and every second dot, a centre line
and a diagonal (observed in the data). Everything showing A-L, the status
row's SETUP included, shows these glyphs while the test runs. Reset leaves
the test: the base set is decompressed again and the state becomes 10h.

The search (op 30) stops at a field attribute (character byte C0-FF), or
ends count cells back when there is none. That is how the runtime's field
lookup at F000:BAAD reads its result (observed). POST's check fits it too.

Other accesses seen during the test, not modelled (open): writes of
`5A`/`A5` and `55`/`AA` to `9A000-9A001` (ROM space, a RAM probe), and
zeros to `62000-620FF` (unmapped).

## Where bring-up stands (2026-11-26)

1. **Test display station shows A-L wrongly (open bug).** The test's character row
   should read `0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ` (owner). In emulation, glyphs
   A0-AB of the base set (main plane `48000`, companion `40000`) are overwritten
   while the test runs. So the character row, the prompt on row 23 and the E of the
   status row's SETUP are garbled (observed). The writer is the font decompressor
   (svc_049, `D000:7A3E`). On entry the test unpacks all four sets again, with the
   same sources and destinations as at boot. It then unpacks small patches from
   `8000:71xx` to destinations such as `4000:0020`, `4000:8020`, `4000:0060` and
   `4000:8060` (observed).

   Next step: log each call's destination and length, find the one that writes
   `49400-4957F`, and check how its copy references resolve. They count from the
   call's destination base. Suspects: a patch that assumes a different base, or a
   mapping (window, memory engine) modelled wrongly (hypothesis).

   Monitor tip: on a breakpoint, use `run #N` rather than `go`; `go` never returns
   once the hits stop.
2. **Memory engine:** operation = `58110` bits 5-3; the search stops at a character
   C0-FF (see Display test). Fixed.
3. **CUT controller** (`src/host/coax/cut_controller.cpp`): written and wired into
   HostLink and Application (a coax model connects with the same host, port and TLS
   settings). It has not yet run against the emulated 3483. Next: run it there,
   then against a TN3270 host.
4. **Then the 3472:** refactor the board into a coax-terminal device shared with the
   3483.

To reach the test from a first boot (keys as `tests/rc/3483-smoke.rc` uses them):
Reset (`lctrl`) at the NVR line, then Down four times and Enter (`rctrl`) in
Offline Setup. Reset leaves the test.

## Open questions

- Why the next boot opens Offline Setup although a valid setup is saved
  (the runtime's state byte `[06FC]` is 10h in that loop, hypothesis: the
  setup state). It may be the
  absent coax signal at power-on; it may need another setup step (the
  serial number is blank on a new unit).
- What the normal online state shows before a controller polls: in
  emulation, a blank screen and a status row whose first six cells are
  blank and the rest a reverse bar (observed), with no cursor.
- `5801C` bits 2, 12 and 13; `58014`, `58022`, `58024`, `58018`, `58028`.
- The status row's address; the row-list entry fields besides A bit 7.
- The printer port.
- What the letters `M`, `D` and `C` of the POST error line stand for.
- The colour-table drive levels against the photographed palette.
