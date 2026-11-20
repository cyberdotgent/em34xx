# 3477 bring-up log

Hardware facts and hypotheses for the 3477, as worked out from running the
firmware. Each entry is labelled **observed** (seen in emulation or on
hardware), **documented**, or **hypothesis**. This file describes hardware
only: register addresses, bit meanings, reset values and protocols.

## Memory map

| Range | Contents | Status |
| --- | --- | --- |
| `00000-1FFFF` | RAM, 128 KiB | Observed: the firmware tests and clears all of it. Includes the character window (`08000-17FFF`, cell offsets in the display registers are relative to `08000`) and the character-generator RAM (`18000-1FFFF`). |
| `20000-200FF` | System gate array (registers at `20000-2003F`) | Observed. |
| `20100-201FF` | Display controller (registers at `20100-2013F`) | Observed; see `src/devices/ibm3477/display.hpp`. |
| `40000-7FFFF` | Optional RAM, absent | Observed: probed by a write/read-back on every boot. Mapped as open bus (reads `FF`, writes ignored) so the probe is not an unexpected access. |
| `A0000-BFFFF` | Optional feature ROM, absent | Observed: a signature word near the top is read on every boot. Mapped as open bus. |
| `C0000-DFFFF` | ROM `95F4172` | Observed. |
| `E0000-FFFFF` | ROM `95F4171`, reset vector | Observed. |

No port I/O is used, and no 80186-only opcode is executed (observed), so an
8088 core is sufficient. The CPU type and clock are still board-level
questions; 8 MHz is a pacing placeholder.

## System gate array (`20000`)

Registers are 16 bits wide at even addresses. Where a register belongs to
the two Twinax channels the even byte is channel 0 and the odd byte channel 1
(observed: per-channel code reaches its byte through a pointer to `20000` or
`20001`).

| Address | Dir | Function | Status |
| --- | --- | --- | --- |
| `20000/1` | R/W | Twinax channel control (ch 0 / ch 1), see [Twinax engine](#twinax-engine-20000-20009-20015-20020-2002b) | Observed access, hypothesis meaning. |
| `20002/3` | R | Interrupt request register (16 bits); also read as per-channel status | Observed. |
| `20002/3` | W | Acknowledge: write 1 to clear | Hypothesis, strong (`FFFF` clears all at init; handlers write their own bit). |
| `20004/5` | R/W | Interrupt enable mask, 1 = enabled; readable | Observed use; bit map hypothesis. |
| `20006` | W | Written `FF` at init; function unknown | Stored. |
| `20007` | R/W | Bit 0: keyboard interrupt enable | Observed. |
| `20008/9` | W | Twinax station addresses (`20008`) and engine control (`20009`), see below | Observed use, hypothesis meaning. |
| `2000A` | R | Configuration straps, see below | Observed. |
| `2000A` | W | Timed output latch, function unknown | Stored. |
| `20010` | W | Status/alarm latch: bit 7 = alarm tone on, bits 6-0 = tone setting; `9B` while booting or after a fatal error | Hypothesis, strong. Tone changes are logged (`sound` category). |
| `20011` | W | Alarm companion | Stored. |
| `20012-20014` | | Keyboard interface, see below | Observed. |
| `20015` | W | Bit 1 = speaker gate (click/beep codes); bit 0 set at end of POST; bits 7, 6, 4, 3, 2 = Twinax loop-back test control | Hypothesis. |
| `20020-2002F` | R/W | Twinax engine loop-back test registers: transmit DMA address (`20020`) and count (`20022`), frame generator (`20024-28`), receive latch (`2002B`) | Hypothesis. |
| `20030` | W | Printer port data | Hypothesis, strong. Stub. |
| `20031` | R/W | Printer port control (bit 0 strobe, bit 1 init) / status (low nibble) | Hypothesis, strong. Stub: reads back the last control value. |

Everything else in the page is plain storage.

### Interrupts

There is no 8259 and no end-of-interrupt write (observed). Each source has a
fixed vector, presented on the interrupt-acknowledge cycle. Bit assignments
are a hypothesis consistent with every mask value the firmware uses:

| Source | Request | Enable | Vector |
| --- | --- | --- | --- |
| Timer | `20002` bit 15 | mask bit 15 | `06h` |
| Twinax ch 0 event | bit 0 | mask bit 0 | `07h` |
| Twinax ch 1 event | bit 8 | mask bit 8 | `08h` |
| Twinax ch 0 status | bits 1-6 | mask bits 1-6 | `09h` |
| Twinax ch 1 status | bits 9-14 | mask bits 9-14 | `0Ah` |
| Display | display controller (`20136` acknowledges) | none identified (always enabled) | `0Ch` |
| Keyboard | receive status (`20014` clears) | `20007` bit 0 | `0Eh` |

Priority, highest first, is the table order. Requests are levels: a source
stays pending after the acknowledge cycle until it is cleared, which matches
handlers that first narrow the mask and then enable interrupts (observed).
Vectors `00h-0Eh` are the only ones in use; NMI is not used (observed).

### Timer

A fixed-rate tick sets request bit 15. The rate is not programmable
(no rate register is written). **50 Hz** is a strong hypothesis: the firmware
counts an hour meter with prescalers that give exactly one increment per
hour at that rate. The emulator derives it from CPU cycles and the model's
clock (`clock_hz / 50`).

### Configuration straps (`2000A`)

| Bits | Meaning | Status |
| --- | --- | --- |
| 0 | Test/diagnostic strap; 0 for normal operation | Observed (read), meaning hypothesis |
| 3-1 | Display type. Only `010`, `110` and `111` (byte values `04`, `0C`, `0E`) are accepted; anything else makes POST beep for ever | Observed |
| 5-4 | Boot mode: `00` normal; `01`, `10`, `11` select manufacturing/burn-in modes | Observed (mode select), meaning hypothesis |

Mapping of the `--display` subtype (hypothesis, no hardware evidence yet):

| Subtype | Straps |
| --- | --- |
| green, amber, paperwhite | `0C` (display type `110`) |
| color | `0E` (display type `111`) |

Type `111` gets its own character-cell geometry and a different monitor
control setting in `2012F`, which suggests a different monitor; colour is
the most plausible candidate. Type `010` is not selected by any subtype. The
whole byte can be overridden with `--straps HEX` or `[emulator] straps`.

### Keyboard interface (`20012-20014`)

A byte-wide shift register on the keyboard's clock and data lines, connected
to the set-3 keyboard model (`src/input/set3_keyboard.*`, `docs/keyboard.md`).

| Address | Dir | Function |
| --- | --- | --- |
| `20012` | W | Byte to send |
| `20012` | R | Last byte received |
| `20013` | W | Line control. `03` line held (idle, the keyboard cannot send); `02` line released: a byte written to `20012` is sent and the keyboard may send; `01`/`03` toggling clocks the shift register by hand (wrap test). |
| `20013` | R | Status: bit 0 = received odd-parity bit, bit 1 = busy sending, bit 2 = byte received |
| `20014` | W | Clear the receive status and the keyboard interrupt |

The keyboard interrupt (vector `0Eh`, enabled by `20007` bit 0) is raised
when a byte has been received **and when a byte has been sent** to the
keyboard (observed: the firmware waits for one interrupt per byte it sends,
discards it, and only then expects the keyboard's `FA`). The keyboard cannot
send while either is pending. Each byte takes about 1 ms on the line
(11 bits at the keyboard's 10-16.7 kHz clock).

Observed: the parity bit in `20013` must be the odd-parity bit of the data
byte (set when the byte has an even number of ones), or the firmware asks
for a resend. Hypothesis: bit meanings of `20013` written values, chosen to
match a clock/data line (`02` starts a transmission on the 1->0 edge of bit
0, and receiving is possible only in the released state).

**Wrap test (observed protocol, hypothesis implementation).** With the line
held (`01`), the self-test writes a byte to `20012` and toggles `20013`
between `01` and `03` eleven times (start, 8 data bits, parity, stop). The
byte must then appear in `20012` with the right parity in `20013` bit 0 and
raise the keyboard interrupt. The emulator loops the transmit register into
the receive side on the eleventh 0->1 edge of bit 1.

Keyboard protocol seen on the line (observed): reset `FF` answered by `FA`,
`AA` and the two ID bytes; each ID byte must have bits 7-6 = `10` or `01`
(the emulated 122-key keyboard reports `BF` + the layout byte). After the
reset the firmware configures key modes with the standard set-3 commands:
`F9` (all keys make-only), `FB` + a list of keys (typematic), `F3` + rate,
`FC` + a list (make/break), `F4` (enable), expecting `FA` after every byte.

### Setup EEPROM and the two-wire bus

The display controller register `2012F` bit-bangs an I2C bus (observed:
bit 0 = SDA, bit 1 = SCL, written 1 pulls the line low, read 1 = line is
low; the master honours clock stretching). Devices:

| Address | Device | Status |
| --- | --- | --- |
| `A0/A1` | Serial EEPROM holding the setup configuration (random reads, byte writes) | Observed protocol. Modelled as a 24C02 (256 bytes, 8-byte pages); size is a hypothesis. |
| `8C` | Write-only device, probably the monitor's deflection/geometry controller | Hypothesis. Acknowledges and logs (`i2c` category, debug). |

The configuration is checksummed (pairs of bytes and blocks that sum to
`EA`); a blank EEPROM fails the check and the firmware enters the setup
menu, as a new unit would (observed).

Persistence: `--eeprom FILE` (or `[emulator] eeprom = "FILE"`) selects an
image file. An existing file is loaded and must be exactly 256 bytes; a
missing file starts blank and is created on the first write. Every write
transaction is saved when it ends (STOP), and pending changes are saved on
exit. Without an image the EEPROM is **ephemeral**: blank at start,
discarded at exit. The monitor shows its state in `info` and offers
`eeprom`, `eeprom dump` and `eeprom save [FILE]`.

## Twinax engine (`20000-20009`, `20015`, `20020-2002B`)

Implemented in `src/devices/ibm3477/twinax_engine.*` as a generic engine
with N station channels (two on the 3477). All entries are **hypotheses**
built from the firmware's register traffic and the 5250 station protocol
(documented: IBM 5250 Product Attachment Information), unless marked
observed. With this model the POST communications test (`COM`) passes
(observed).

### RAM structures (the engine is a bus master)

The engine's tables live in ordinary RAM, which the firmware fills and the
POST RAM test covers (observed). Buffer addresses are 16-bit linear
addresses (first 64 KiB).

| RAM | Contents |
| --- | --- |
| `0060-007F` | Command-class table, indexed by `command & 1F`: `00` queue, `40` write activate, `80` poll, `C0` read activate. The normal setting gives Read Activate (`00`) `C0`, Activate Write (`01`) `40` and Poll (`10`) `80` (documented command codes); the self-test assigns other codes on purpose (observed). |
| `0080-00FF` | Channel 0 programs, 32 bytes each, one per class (`class >> 6`) |
| `0100-017F` | Channel 1 programs |
| `0180-018F` | Channel 0 descriptor |
| `0190-019F` | Channel 1 descriptor |

Descriptor fields: `+0/+1` activate buffer address, `+2/+3` activate count,
`+4` activate mode (0 = not armed), `+5` free bytes in the command queue,
`+6/+7` queue write address, `+8` station address and fill, `+9` mode,
`+A` function control (bit 2 or 3 set = two-frame poll response), `+B`
polled flag (the poll program clears it), `+C` poll status (exception code
in bits 3-1, `0E` = power-on), `+D` poll byte 2 (keyboard scan code),
`+E` latched activate mode, `+F` zero source.

### Programs

For each message addressed to an enabled channel the engine runs the
channel's program for the command's class, one byte per step, with one
accumulator and a 16-byte register file shared by all channels:

| Opcode | Action |
| --- | --- |
| `2r` | register `r` := accumulator |
| `3n` | descriptor byte `n` := accumulator |
| `4r` | accumulator := register `r` |
| `5n` | accumulator := descriptor byte `n` |
| `8x`/`9x` flags | `08`: if the accumulator is 0, raise invalid activate (`20`) and stop; `02`: queue mode (store the command frame too, raise `02` and `08` when done); `10`: raise activate done (`04`); `04`: end |
| `A1` | transmit `r2/r3` bytes from RAM at `r0/r1` as the response (last frame address 7) |
| `A2` | store the message's data frames at `r0/r1`, at most `r2/r3` bytes (more raise overrun `10`); `r0-r3` are updated |
| `C0` | start the response; for the poll class it is the status response |

Any data transfer sets the channel's busy latch.

### Registers

| Address | Function |
| --- | --- |
| `20000/1` W | Bit 7: drop busy. Bit 4: a new poll byte 2 was posted (the next poll flips the response level). |
| `20000/1` R | Bit 7: busy latch. Bit 3: program running (never seen set here). |
| `20008` | Station addresses: channel 0 bits 2-0, channel 1 bits 5-3. |
| `20009` | Bit 1/2: channel 0/1 enable. Bit 7: engine run (0 = held; held, nothing is taken from the line). Bits 3-6 are written per channel but have no modelled effect. |
| `20015` | Bit 6: loop-back (line disconnected). Bits 3/2: transmit path; while loop-back is on, every write with bit 3 or 2 set sends the frame generator's message. Bit 4: send it with a parity error. Bit 7 (or `20025` bit 5): the message comes from the transmit DMA. |
| `20020/1` | Transmit DMA address, `20022/3` count. |
| `20024` | Frame generator: bits 2-0 = address of the first frame. |
| `20025` | Bit 5: DMA message. Bit 2 or 3: two frames (else one). |
| `20026` | First frame data (command). `20027`: second frame data (address 7). `20028`: written with the same value as `20027`; no modelled effect. |
| `2002B` R | Loop-back receive latch: data of the last frame on the looped line (the station's response, or the last transmitted byte when nothing answers). |

### Channel events

Request bits in the gate array (`20002` for channel 0, `20003` for
channel 1; see [Interrupts](#interrupts)): `01` poll response sent (vector
`07h`/`08h`), `02` command queued, `04` activate done, `08` end of a queued
message, `10` overrun, `20` invalid activate, `40` line parity error (vectors
`09h`/`0Ah`).

### Poll response

Status byte: bit 7 busy, bit 6 line parity error, bit 5 toggles on every
response (the self-test checks this), bits 4-1 from descriptor `+C`, bit 0
response level. Busy = the busy latch or any unacknowledged request bit of
the channel (this makes the self-test's second poll read busy, observed
requirement). A poll with bit 6 (`40`) resets the line parity error first.
After a parity error the channel ignores everything but polls. With `+A`
bits 2-3 clear the response is one frame with address 7 (power-on, before
Set Mode, documented); otherwise two frames: status with the station
address, then byte 2 (register 7 for channel 0, 8 for channel 1) with
address 7.

### Line timing

The line runs at 1 Mbit/s: a message takes about 8 + 16 x frames bit times,
the station answers 45 bit times after the end of the command (documented:
45 +- 15 us), all counted in CPU clocks from the model's clock. The engine
follows the `TwinaxHost` contract: it takes a message only when the line is
idle and the engine runs, and reports either the response or
`no_response()`. Loop-back messages are processed at once.

### Bring-up transcript (script host)

Observed, firmware past POST with the failed display test skipped by hand
and a blank EEPROM, `--twinax-host script`:

```
twinax send 30/0      # Poll with ACK to station 0
twinax recv           # no messages from the station
twinax status         # 1 message(s) queued, 0 delivered
```

The firmware is in Offline Setup and leaves the engine held (`20009`
bit 7 = 0), so the message is never taken from the line; this is the
expected behaviour of a held engine. Per the model, once the firmware runs
the engine with station 0 configured, the same poll is answered with one
frame `0e/7` (power-on exception, not busy) until Set Mode; after
`13/0 00/7` and the firmware's handling, polls are answered with two frames
`xx/0 byte2/7`.

## Boot progress with real devices (no forced results)

Observed with `--display green`, an ephemeral EEPROM and no Twinax device:

1. Reset, register-only RAM test, ROM checksums, strap decoding.
2. POST tests: RAM passes and the communications loop-back test (`COM`)
   passes with the Twinax engine model (observed). POST then stops at the
   display test (`VDO`) with the alarm tone on (`20010` = `9B`) and the CPU
   halted.

Later stages, observed only by letting the failed tests continue from the
monitor (bring-up exploration, not a product setting):

- the display test (`VDO`) fails on the monitor sense nibble (`2012F` bits
  7-4), whose expected values are not understood yet;
- the keyboard wrap (`KWP`) and keyboard (`KBD`) tests pass;
- the main firmware installs its handlers for vectors `06h`, `07h`, `09h`,
  `0Ch` and `0Eh`, runs the 50 Hz timer, completes the keyboard reset, ID and
  key-mode handshake, and then sends `EE` (echo) to the keyboard
  periodically as a presence check;
- with a blank (or invalid) EEPROM it shows the **Offline Setup** menu
  ("Customize workstation", "Update serial number", "Test workstation"),
  as a new unit would.

## Board

- **Observed (owner, 2026-11-20):** the link-side gate array is a Toshiba
  TC-family part, with SRAMs and line drivers similar to those on IBM's ISA
  5250 emulation adapters (whose Enhanced adapter uses a Toshiba TC110G26AF
  gate array with a byte-wide SRAM and a 16-pin line-interface part). This
  supports the hypothesis that the terminal's Twinax engine is the same
  engine design wired directly onto the CPU bus.

## Next steps

1. Twinax engine: check the poll, Set Mode and activate handshake against
   the running firmware once it gets past Offline Setup (needs a valid
   setup EEPROM).
2. Monitor sense nibble in `2012F` for the display test.
3. Confirm the strap/display-type mapping and the CPU type and clock on a
   board.
