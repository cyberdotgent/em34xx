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
| `00000-3FFFF` | DRAM (two 256Kx4 parts). `0:0800` local status buffer, `0:0A00` status row, `0:1000` regen buffer | parts observed on the board; placement hypothesis |
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
and leaving setup gives 32 rows of 13-line cells, linear from `0:1000`
(observed). Before the row list below was worked out, row 28 repeated row 0.

### Screen rows and the status row

`5801A` bit 6 set: a linear screen from `0:1000` (POST's screens and the
setup screens, observed). Clear (the online runtime, POST test 7): each row's
start comes from the row list in block RAM. Entry e is at `50108+4e` (A)
and `50508+4e` (B). An entry with A bit 7 clear names the cell
`((A & 7F) << 8) | B[0]`, biased by 800h cells (observed: the firmware
adds the 800h itself). An entry with bit 7 set continues after the previous
row.

The list runs downward. The runtime's builder (`D000:4F50`, `D000:513C`)
writes entries 0-61 (observed):
- the default "continue" entry (`85`, B `00 01 7C`) everywhere;
- row r's start at entry 62 - r for r = 1 to rows - 1;
- two "continue" entries below the last row;
- the regen origin (`08 00`) under them, at entry 60 - rows.

It writes the same layout in 24, 32 and 43 rows, so the `0800` lands at
entries 36, 28 and 17. Reading entry r for row r repeated row 0 at row 28
in 32x80 (and would have at row 17 in 43x80). The model takes the walk to
start at entry 61 during row 0, with the entry read during a row setting
the next row's start. So row r >= 1 comes from entry 62 - r, and row 0
from the lowest entry below the screen with bit 7 clear (the last one read
during vertical blanking) (hypothesis). It fits the runtime in all three
sizes and all six of POST test 7's sense readings, whose lists repeat one
entry. B[2] is minus the row length (`7C` = -132, `B0` = -80, observed
values). B[1] bit 0 and A's other bytes are not understood (open). With
`[es:bx+6]` nonzero the builder starts at a later window row (open: where
the runtime uses that).

The status row (the 3270 operator information area) is drawn from the
132 cells at `0:0A00` (the screen's width of them), below the screen with a
thin rule along its top edge. Photos of a real 3483 show a "SETUP" bar under
a blue line on every setup screen (observed). The firmware builds the row
in one routine (`D000:D03A`, run when `[0784]` bit 2 asks for it) in a
132-cell frame on its stack and copies the frame to `0:0A00` (`D000:D31F`,
far pointer at `D000:D53A`) (observed):

- on line (`[07B8]` = 0): the controller's cells 0-79 from the coax buffer
  (`0:C000`, pointer `D000:D532`), with its own indicators over them, among
  them the cursor position (`0005/001`) in cells 72-79 from `[0752]`, and
  the colour bits of every EAB set to blue (`08`, or green `20` when
  `[031B]` is not 4);
- in setup and the other local states (`[07B8]` nonzero): its local status
  buffer at `0:0800` (pointer `D000:D536`), 132 cells, where setup writes
  "SETUP" in reverse white (`F000:62CB`);
- with `[06FD]` bit 5 set (the screen saver, see below): a blank row with
  one symbol in cell 71 (`1F`, or `A3` for the values 28h, 37h and 24h of
  `[0003]`).

The display mode set clears `0:0A00` (84h words of `2140`) together with
the regen buffer (`D000:48E8`, `D000:4B05`), and after POST the CPU never
reads `0:0A00` back (observed: a read watchpoint over setup and the online
state). So `0:0A00` is the row the controller shows; `0:0800` is a
firmware buffer. The cell at `0:0A00` is not named by any register or
row-list entry seen; that the address is fixed in the hardware is a
hypothesis. Until 2026-11-27 the model drew `0:0800`, which showed SETUP
but never the controller's status line.

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

### Screen saver

`[06FD]` bit 5 is the screen saver (observed, 2026-11-27). The timer
routine counts `[07A4]` up once per tick (`F000:A5D5`) while setup byte
`3C5` (item 26h) is nonzero, and when the count reaches `3C6` (item 27h)
times 298h it sets the bit (`F000:A633`) and asks for a redraw (`[0784]`
bits 2 and 13). A key resets the count (`D000:BDDA`); a key while the bit
is set clears it (`D000:BE4E`), asks for the redraw and is not acted on
otherwise (observed). An empty battery-backed RAM gives `3C5` = 1 and
`3C6` = 0Ah (observed). In emulation a tick is about 90 ms (2878 ticks in
2.6e9 cycles at 10 MHz, observed), so 298h = 664 ticks is a minute and the
default is ten minutes (hypothesis, strong: the firmware's own constant).
Neither item appears in Customization (observed); where it is set is open
(hypothesis: Display Setup, with the alarm and clicker volumes, items 28h
and 29h).

While the saver is on, the status routine draws the blank row with the
cell-71 symbol, and the display update (`F000:B64E`) sets `5801A` bit 6
(the shadow `[026C]`), which it also sets for `[079B]` and a local flag
(observed). The bit also selects the linear screen in the model (POST's
screens set it), so the model's picture keeps the setup text: on a real
unit the saver presumably blanks the screen, which bit 6 alone cannot do
if POST's text shows with it set (open). What symbol `1F` shows is open.

This was first taken for a keyboard fault: the boot scripts idle about 17
emulated minutes in Offline Setup (`run #400000000` after Reset), so the
saver was on when they looked, and the first key "was swallowed". Right
after Reset the status row shows SETUP (`tests/rc/3483-smoke.rc` checks
both, and needs three Downs after the waking key to reach the display
test). It is not caused by the keyboard's unsolicited ID bytes after its
BAT result (a separate issue with the boot path).

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
- The underline cursor reverses its lines rather than lighting them
  (hypothesis: mixed into the video like the block cursor). The underscore
  highlight uses the same lines in the same colour, and a 3270 cursor
  nearly always sits in an underscored input field, so a lit underline
  cursor could not be seen there: on line it looked as if no cursor was
  drawn (observed in emulation, 2026-11-27, at moshix.tech's User field).
  Reversed, it shows as a gap in the underscore and as a line elsewhere.
  How a real 3483 shows the cursor over an underscore is open.
- On line the firmware places the cursor itself from the coax address
  counter: after the controller's last LOAD ADDRESS COUNTER, `58010` holds
  the counter's position less the 80-cell status line (observed: counter
  `0529`, `58010` = `0F29`, row 15 column 41) and `5801C` bit 7 is set
  (`3084`). The status row's `0016/042` is the same position, counted
  from 1.
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

Video sense (`58020` read, POST test 7): the firmware clears the count,
enables it with `5801A` bit 5 for one frame, and reads how many of the
guns selected by `58021` bits 0-2 were lit (observed, `C000:C7C1`). The
expected count is in AH (`C000:C5CE`). Test 7 loads glyphs 0 (blank), 1
(only companion bits 3-2: no visible dot) and 2 (solid) through the window.
It writes test cells such as glyph 02 in colour 4 at cell 1F5, and loads
its own drive table with levels just below (`B4`, `C0`) and just above
(`D8`, `DC`, `E0`) one point. Selector 3 over a screen of colour-4 cells
(`70`/`C0`/`DC`) must read 0. So a gun counts as lit only above a threshold
between `C0` and `D8`; the model uses `CC` (hypothesis for the exact
point; a comparator in the sense circuit is the likely hardware).

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
- The 40000 write window: while `58021` bit 7 is set, a word written at
  40000+w is glyph row w/2 of the character generator. Its low byte goes
  to 40000+w/2 and its high byte to 48000+w/2, so the 32 KiB window
  covers the base and APL/GE sets with their companion planes. Observed:
  POST test 4 (`C000:A674`) writes 20h words of `0A5A` at offsets 0-3E and
  checks 20h bytes of `5A` at 40000 and 20h low nibbles `A` at 48000. POST
  test 7 and the display test address character c, row r at c*64 + r*2.
  Reads pass through to the SRAM (hypothesis). `58021` bits 0-2 are not
  part of the window: they keep the video sense's last selection.
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

The page's colour bars are rows of A (code A0) in blue, red, reverse
pink, underscored green, turquoise, yellow and white (observed in
emulation: the cells are A0 with EABs 08, 10, 98, E0, 28, 30 and 38).

On entry the test unpacks the four character sets again, then writes its own
glyph rows through the 40000 window (`E000:1392`, see below). It
addresses character c, row r at window offset c*64 + r*2 (observed). The
model first wrote each byte twice at 40000 + w, which sent those glyphs to
codes 80-AB and garbled A-L, the status row's SETUP and the prompt on row
23. With the window at half the address they land on 40-55, and the page
reads `0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ` (observed in emulation,
matching the real unit). Reset leaves the test: the base set is decompressed
again and the state becomes 10h.

The search (op 30) stops at a field attribute (character byte C0-FF), or
ends count cells back when there is none. That is how the runtime's field
lookup at F000:BAAD reads its result (observed). POST's check fits it too.

Other accesses seen during the test, not modelled (open): writes of
`5A`/`A5` and `55`/`AA` to `9A000-9A001` (ROM space, a RAM probe), and
zeros to `62000-620FF` (unmapped).

## Where bring-up stands (2026-11-26)

1. **Test display station: fixed (2026-11-27).** The 40000 window was modelled
   as writing each byte twice at 40000 + w. That sent the test's glyphs onto
   A-L, and POST passed test 7 only because two errors cancelled out. The
   window is now w/2, and the video sense counts a gun above a threshold
   rather than above black (see Display test and the colour table). Found
   with the monitor's logging breakpoints (`bp`/`wp ... do`,
   docs/monitor.md). `tests/rc/3483-smoke.rc` checks the page.
2. **Memory engine:** operation = `58110` bits 5-3; the search stops at a character
   C0-FF (see Display test). Fixed.
3. **CUT controller** (`src/host/coax/cut_controller.cpp`): written and wired into
   HostLink and Application (a coax model connects with the same host, port and TLS
   settings). It runs against the emulated 3483 and a TN3270 host: the screen
   lands from coax cell 80 and the status line from cells 0-79, through the
   firmware's status row at `0:0A00` (observed, 2026-11-27).
4. **Then the 3472:** refactor the board into a coax-terminal device shared with the
   3483.

To reach the test from a first boot (keys as `tests/rc/3483-smoke.rc` uses them):
Reset (`lctrl`) at the NVR line, then Down four times and Enter (`rctrl`) in
Offline Setup. Reset leaves the test.

## Keys on the coax

On line the firmware sends the controller its keyboard's scan-code set 3
bytes unchanged, one per poll response (observed, 2026-11-27, with
`log coax=debug`): `x` gives `22`; Shift gives `12` and, when let go,
`F0 12`; Caps Lock `14`, `F0 14`; typing keys send no break (the
firmware's key modes). Shift and the lock are the controller's business
(documented 3270: the terminal sends key codes). The CUT controller used
to read each byte as a 5250 Enhanced-keyboard code with bit 7 for a break,
so `F0` was a key of its own and the `12` after it a second make: Shift
stayed down for good and Caps Lock toggled twice per press, so lower case
could not be typed once Shift had been used. It now turns `F0` + code into
the break (`CutController::decode_key`), Caps Lock toggles on its make,
and either Shift releases it (documented 3270 behaviour for Shift Lock).
Caps Lock affects letters only (hypothesis: the 3483's key may be a
typewriter Shift Lock, which would also shift digits and symbols; open).
Set-3 make codes above `7F` (some keypad keys) are not translated (open).
`tests/test_cut_controller.cpp` covers it; checked at moshix.tech
(`aBcDe@x` typed with Shift, Caps Lock and Shift again).

## Open questions

- Why the next boot opens Offline Setup although a valid setup is saved
  (the runtime's state byte `[06FC]` is 10h in that loop, hypothesis: the
  setup state). It may be the
  absent coax signal at power-on; it may need another setup step (the
  serial number is blank on a new unit).
- What the normal online state shows before a controller polls: in
  emulation, a blank screen and a blank status row (observed), with no
  cursor. (The "blank start and reverse bar" seen before was `0:0800`, the
  local buffer, which the model drew until the status row was found.)
- The screen saver (see Screen saver): whether `5801A` bit 6 blanks the
  picture, what symbol `1F` is, and which setup page sets items 26h-27h.
- What the controller should write in cells 0-79: a 3270 controller
  writes the operator information area in its own character set; the
  status routine compares cell 0 with `20`, `31`, `2F`, `09` and `BF`
  (`D000:D1EB`) before laying its indicators over the row (meaning open).
  The CUT controller writes plain text in the display code, which shows.
- `5801C` bits 2, 12 and 13; `58014`, `58022`, `58024`, `58018`, `58028`.
- How the hardware finds the status row at `0:0A00` (no register seen);
  the row-list fields B[1], B[2] and A[1-3].
- The printer port.
- What the letters `M`, `D` and `C` of the POST error line stand for.
- The colour-table drive levels against the photographed palette.
