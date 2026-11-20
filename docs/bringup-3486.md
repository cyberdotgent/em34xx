# 3486 bring-up notes

Hardware facts and hypotheses for the IBM 3486 InfoWindow II, worked out
from the **partial** dump of its ROM (`54g0011.bin`, see `roms/ROMS.md`)
and compared with the 3477 (`docs/bringup-3477.md`). Each entry is labelled
**observed** (seen in emulation or in the firmware's use of the hardware),
**documented**, or **hypothesis**. This file describes hardware only.

Only segments `A000`, `B000`, `E000` and `F000` of the ROM are available.
`8000`, `9000`, `C000` and `D000` are missing, and POST calls into `D000`
early, so everything below comes from the surviving half and from running
POST up to that call. The model (`src/models/model_3486.cpp`) stays
**ROM-incomplete** and runs only with `--allow-bad-rom`.

## Summary

The 3486 is a 3477 derivative. It has the same CPU class, the same gate
array and display controller bases, the same keyboard, Twinax, printer and
I2C register use, and some new registers in the unused parts of both pages
(observed). One ROM serves three products: the VPD model table lists
**3487, 3486 and 3488**, and the configuration straps select the model and
the tube (observed). It also has a second table, `3476/3477`, with the 3477
suffixes (hypothesis: a 3477-compatibility identity).

## CPU

- 8088-class (observed): no 80186-only opcode anywhere in the code reached
  by recursive descent from the reset vector (about 35,000 instructions).
  The code is generated for an 8086 (repeated single-bit shifts instead of
  shift-by-immediate), and there is no port I/O. All hardware is memory
  mapped, as on the 3477.
- **The CPU is an NEC V20** (D70108L-8, 8 MHz part), U12, PLCC-44 (observed,
  board photo). It runs 8088 code, but differs from an Intel 8088 in some
  undocumented opcodes, AAM/AAD, its `0F` extended-instruction prefix and
  timing. None of that is modelled yet; the firmware doesn't need it so far.
- Clock: the only oscillator is a **30.000 MHz** module (Y1, IBM 0666269;
  observed). A CPU clock of 7.5 MHz (30/4) is a hypothesis that fits the
  delay-loop estimate of about 7 MHz; the model uses it for pacing.

## Logic board (observed, photo of a 3486 board, IBM 9325)

| Ref | Part | Notes |
| --- | --- | --- |
| U11 | IBM 0666317 / Toshiba TC140G54AF, large QFP | Custom gate array. Almost certainly the system logic, display controller and Twinax engine in one chip (hypothesis); the Toshiba TC-family lineage matches the 3477 and IBM's 5250 adapters. |
| U12 | NEC D70108L-8 (V20) | CPU, 8-bit bus. |
| Y1 | 30.000 MHz oscillator, IBM 0666269 | Only clock source. |
| U15 | Mitsubishi M5M51008BFP-10LL, 128K x 8 SRAM | Low-power part next to the battery position BT1: the battery-backed RAM (hypothesis). |
| U14 | Sony CXK58257AM-70L, 32K x 8 SRAM | Fast SRAM, possibly display or character-generator memory (hypothesis). |
| U4, U5 | Mitsubishi M5M4C264AL-10, ZIP | Multiport video DRAM, 64K x 4 each = 64 KiB display memory (observed by the dumper; corrects an earlier guess of 256 KiB general DRAM). |
| U2 | Sharp mask ROM 54G0011 (c) IBM 1993, 32-pin DIP | The firmware ROM, 256 KiB (removed for dumping when photographed; corrects an earlier guess that U2 was an empty optional-ROM footprint). |
| U8, U9 | 74ALS273, 74LS373 | Address/data latches for the multiplexed CPU bus. |
| U6, U7, U13, U16 | 74F38/74LS38-type, 74LS14, 7406 | Open-collector drivers and Schmitt-trigger receivers for the external ports. |
| U10 | National M9324, IBM 33G2729, 16-pin DIP | Unknown. |
| U17 | Bell hybrid 38F5775 (LM431, 2903 comparator, discretes) | Twinax line driver/receiver (observed by the dumper). |
| PS1 | Optical sensor | Keylock detector (observed by the dumper). |
| J1 | DE-15 with varistors RV1-RV4 and Q1 | Twinax port with surge protection (hypothesis). |
| J2 | DB-25 | Parallel printer port. |
| J6, J4 | Modular jacks with TDK filters | Keyboard, and the **pointing-device port**: a PS/2 mouse or a light pen (observed by the owner). This is the second keyboard-style serial port at `20144-20146`. |
| J5, J7 | Card-edge and pin headers | CRT/power board connections. |

The board's ROM label reads `54G0015`; the analysed dump is `54G0011`, so
several firmware levels exist.

## Memory map

| Range | Contents | Status |
| --- | --- | --- |
| `00000-5FFFF` | RAM | Observed in use up to about `52000`: stack and data at `38000-3C07F` (tested by the POST pre-test), Twinax data buffers in segment `5000`, a checksummed block at `469E0`, buffers cleared at `3F080` and `41082`. 384 KiB is a **hypothesis** that the board does **not** obviously support: the visible RAM is 128 KiB (U15) + 32 KiB (U14) SRAM, and U4/U5 are 64 KiB of video DRAM. How `00000-5FFFF` is decoded is unknown. |
| `20000-200FF` | System gate array | Observed (same base as the 3477). |
| `20100-201FF` | Display controller, now with registers up to `2014F` | Observed. |
| `60000-7FFFF` | Optional 128 KiB ROM | Observed: POST checksums segments `6000-7000` (word sum `619A`) only when strap `2000A` bit 6 is 0, and copies 7 bytes from `60050` into the setup block. Absent (open bus) in the model. |
| `80000-FFFFF` | ROM `54G0011`, reset vector at `FFFF0` | Observed. |

ROM integrity (observed): POST sums the 16-bit words of segments `8000`
to `F000` and requires `619A`. A flag word of `E13C` at `F000:0003`
skips the check (not set in this ROM). The surviving half sums to `DBE9`,
so **the four missing segments must sum to `85B1`**. This is a quick
check of the re-dump.

RAM areas (observed, named after the 3477 equivalents): the system state
block is at `0500` (3477: `0AA0`), the setup/configuration block at `0280`
(3477: `01A0`), another state block at `0560` (3477: `0A20`).

## Configuration straps (`2000A`)

The 3486 decoder differs from the 3477 one (observed):

| Bits | Meaning |
| --- | --- |
| 3-1 | Display type, see below |
| 5-4 | Boot mode: `00` normal, `01`/`10`/`11` manufacturing or test modes. Bits 3-1 = `101` with `2012F` bits 1-0 = `11` also selects a test mode. |
| 6 | 0 = optional ROM at `60000` fitted (POST checks it) |
| 0 | Read by the later boot-mode code, as on the 3477 (meaning hypothesis) |

Display type decoding (observed). "Family" is a second value the firmware
keeps with the type; the VPD suffixes come from the table at the top of
`F000`:

| Bits 3-1 | `2012F` bit 1 | Type | Family | VPD suffix |
| --- | --- | --- | --- | --- |
| `001` | 1 / 0 | 1 / 2 | 1 | `BA0` (3486) |
| `101` | 1 / 0 | 3 / 4 | 1 | `HG0` / `HA0` (3487 green / amber) |
| `100` | - | 5 | 1 | `HW0` (3487 white) |
| `110` | - | 6 | 2 | `HC0` (3487 colour) |
| `010` | - | 7 | 3 | `XXX` (3488, which drives an external monitor) |
| `011` | - | `80` | `FF` | rejected |
| other | - | 3 | `FF` | rejected (POST beeps for ever) unless a test boot mode is selected |

Model numbers per family (3486 for types 1/2, 3487 for types 3-6, 3488 for
type 7) are a **hypothesis**: the code that indexes the table is in a
missing segment. The model uses straps `42` (type 2, no optional ROM;
hypothesis for a real 3486). The `--display` subtype does not change them.
`--straps` overrides the byte, so the 3487 codes can be tried.

Compared with the 3477, `0C` now means colour (3487 HC0), and `0E` is
rejected.

## System gate array (`20000`)

Same use as on the 3477 for `20002-2000A`, `20010-20015`, `20020-2002B`
and `20030-20031` (observed). The new registers (observed accesses,
**hypothesis** meanings):

| Address | Use |
| --- | --- |
| `2000D` | Written `FF` when the third Twinax channel is set up. Hypothesis: channel 2 request register (write 1 to clear), the counterpart of `20002/3`. |
| `2000F` | Saved and restored together with the 16-bit mask `20004` by the interrupt code, written 0 at channel 2 set-up. Hypothesis: channel 2 enable mask (third mask byte). |
| `20017` | Written `20` at reset. |
| `2001A` | Station address of the third Twinax channel (written next to `20008`, bits 2-0). |
| `2001B` | Written by the Twinax and loop-back test code; channel 2 counterpart of `20009` (hypothesis). |
| `2002A` | Written by the loop-back test (next to the frame generator `20024-28`). |

`20006` (written `FF` on the 3477) is not written by the surviving code.

## Display controller (`20100`)

- Same register block as the 3477 (timing block `00-17`, mode block
  `25-2E`, `2F` status/I2C, window starts, interrupt acknowledge `36`,
  block request `38-3B`) (observed).
- Timing values differ (observed). The horizontal timing registers
  `00-05` are smaller and register `0E` (vertical blanking) is `1C`
  instead of `1A`. In the 24x80 format the frame is 553 lines, against 551
  on 3477 types 3/4. So the 3486 tube has different timing (hypothesis:
  this is not visible to the operator, and the user's photos show the same
  screen layout as a 3477).
- `2012F` is written `00` at reset (3477: `18`). Bit 1 is read as a sense
  input during display-type decoding (see the straps).
- New registers at `20140-2014F` (observed accesses):
  - `20140` (`02`), `20141`, `20149` are written with the timing block.
    `20141` is also written in many display mode changes (hypothesis:
    monitor power or blanking control, the Energy Star feature).
  - `20142` is written `FF` once.
  - `20144-20146` are a **second keyboard-style serial port** with the same
    protocol as `20012-20014`. `20144` is the data byte. `20145` is
    line control: written `02` released, `03` held. Its read bits are
    bit 0 parity, bit 1 busy, bit 6 byte received, and bit 7, see below.
    `20146` is written `FF` to clear. The firmware runs a keyboard-type
    exchange on it. The attached device is unknown (hypothesis: a pointing
    device or a power-management controller).

### `20145` bit 7: interrupt layout

The firmware reads `20145` bit 7 in the keyboard, Twinax and interrupt
code and switches layout on it (observed):

| Bit 7 | Keyboard (`20012`) | Second port (`20144`) | Third Twinax channel |
| --- | --- | --- | --- |
| 1 | vector `0Eh` (as the 3477) | vector `0Dh` | not set up |
| 0 | vector `0Fh` | vector `0Eh` | set up (descriptor `01A0`, programs `0200-027F`, `2000D/F`, `2001A`) |

Which value a 3486 gives is **unknown**. It may be a board-revision or
product (3486/3487) input. In the model the display page stores `20145`,
so bit 7 reads 0, and the keyboard handshake then raises vector `0Eh`
while the firmware waits on `0Fh` (observed). With bit 7 forced to 1 in a
private experiment, the existing keyboard interface completes the reset/ID
handshake (`FF` -> `FA`, `AA`, `BF BF`) (observed).

## Twinax

- Same engine design: command-class table at `0060`, channel 0/1 programs
  at `0080-017F`, station addresses in `20008` (observed).
- A **third station channel** exists in the firmware (observed): programs
  at `0200-027F`, descriptor at `01A0`, station address in `2001A`.
  Station-address selection (observed): three addresses are configured.
  Depending on a setup mode, channel 1 gets the second or the third
  address, and the third goes to `2001A`. Hypothesis: two display sessions
  plus one printer session, each on its own channel.
- The model keeps the 3477's two channels: two displays, or display +
  printer.

## Battery-backed RAM and EEPROM

- Not yet established for the 3486. The setup block moved to `0280`. The
  model keeps the whole RAM in the `--nvram` image (384 KiB) and the I2C
  EEPROM at `A0` as on the 3477 (hypothesis).
- The IBM 3489 maintenance manual (SY18-2345, a later member of the
  family) documents a "ROS patch" utility that stores patch data in NVM
  (documented for the 3489). The checksummed block at `469E0` (length word,
  data, sum `619A`) may be the 3486's patch area in RAM (hypothesis).

## Boot progress with the partial dump

Observed with `--model 3486 --allow-bad-rom` (default straps `42`):

1. Reset at `FFFF0` -> `F000:0000` -> `F000:01DF`: gate array masked, alarm
   latch `9B`, display registers initialised, `20017` = `20`.
2. Register-only RAM pre-test of `00500-0055F` and `39E80-3C07F` passes.
3. ROM checksum: the first read of segment `8000` is an unmapped access,
   so the machine stops there. With `policy log` the sum is `DBE9`, POST
   beeps and halts.
4. Skipping the checksum by hand (`bp f000:2b88`, `go`, `regs ip=2bcc`,
   `go`), POST decodes the straps and the display type, waits for vertical
   retrace, loads the display timing and mode blocks and `20140/41/49`.
   On types other than 6 it then resets the keyboard. As modelled
   (`20145` bit 7 = 0), the keyboard interrupt arrives on vector `0Eh`,
   which is not installed yet, and the CPU runs into zeroed RAM. With
   bit 7 = 1 (experiment), or with type 6 (`--straps 0c`), POST reaches
   `call D000:13B6`, the first call into a missing segment.

The model maps the four good 64 KiB blocks at their linked segments
(`A000`, `B000`, `E000`, `F000`) and leaves the others unmapped. This
applies only when the known-bad dump is loaded; a good dump is mapped
whole at `80000`.

## Pointing device and the enhanced user interface (observed by the owner)

The second port takes a **PS/2 mouse or a light pen**. With a mouse plugged
in before power-on, the terminal shows a mouse pointer, has clickable
hotspots, and menus can be driven with the mouse. The 5250 "enhanced"
windowing elements (windows, selection fields, scroll bars and the like,
sent by the host as Write Data Structured Field orders) also work with it.

Consequences for the emulator:
- the second port at `20144-20146` needs a PS/2 mouse device model
  (standard PS/2 mouse protocol on the same serial interface as the
  keyboard), fed from host mouse events; detection happens at power-on;
- the `20145` bit 7 interrupt layout switch is probably tied to whether a
  pointing device is present (hypothesis);
- the TN5250 engine currently parses and skips Write Data Structured
  Field; the enhanced interface needs it implemented, and the Query Reply
  must only claim it for a terminal that supports it.

## Checklist for the complete dump

1. Verify: 524288 bytes; no invariant address bit; the word sum of the
   whole image is `619A`; blocks 2/3/6/7 (or 0/1/4/5, by the re-dump
   method) match the current `A000/B000/E000/F000` content. Record the
   SHA-256 in `roms/ROMS.md` and in `info_.roms`, and drop the known-bad
   mapping path if no longer needed.
2. Set the model status to `Bringup`.
3. RAM size: find the POST RAM test (probably in `C000/D000`) and the
   highest address it touches; adjust `kRamSize`. Check whether
   `60000-7FFFF` is probed as RAM or only as the option ROM.
4. `20145` bit 7: decide from the code in the missing segments (or from the
   unit) which layout a 3486 uses. If 0: keyboard on vector `0Fh`, the
   second port on `0Eh`, three Twinax channels (see the proposals below).
5. Fonts: no 3477-format compressed font plane is present in the surviving
   segments (observed). Look for them in `8000/9000/C000/D000`, and check
   the character-generator RAM location (`18000-1FFFF` on the 3477; this
   firmware references `1800:0000` too).
6. Display: confirm the `20140-2014F` registers (power saving/blanking),
   the VPD model-number indexing, and how `2012F` bit 1 differs between
   types 1 and 2.
7. POST tests: list the 3486 POST test table and its display test.
   Check whether the video-signature test of the 3477 (`2012F` bits 7-4)
   is still there.
8. Setup: locate the setup block validation (block at `0280`) and the
   battery-backed range; check the EEPROM use.
9. Compare against the real unit: the "3486 Control Program" logo screen
   and the sign-on screen with the status line (owner's photos).
10. Sessions: confirm two display sessions and the printer session on a
    real 3486 (Read Info VPD, Read Device ID model ID).
