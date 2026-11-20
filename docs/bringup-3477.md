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
| `20000/1` | W | Twinax channel command (ch 0 / ch 1) | Observed access, hypothesis meaning. Stub. |
| `20002/3` | R | Interrupt request register (16 bits); also read as per-channel status | Observed. |
| `20002/3` | W | Acknowledge: write 1 to clear | Hypothesis, strong (`FFFF` clears all at init; handlers write their own bit). |
| `20004/5` | R/W | Interrupt enable mask, 1 = enabled; readable | Observed use; bit map hypothesis. |
| `20006` | W | Written `FF` at init; function unknown | Stored. |
| `20007` | R/W | Bit 0: keyboard interrupt enable | Observed. |
| `20008/9` | W | Twinax configuration (station/mode; `20009` bit 7 = engine run) | Hypothesis. Stub. |
| `2000A` | R | Configuration straps, see below | Observed. |
| `2000A` | W | Timed output latch, function unknown | Stored. |
| `20010` | W | Status/alarm latch: bit 7 = alarm tone on, bits 6-0 = tone setting; `9B` while booting or after a fatal error | Hypothesis, strong. Tone changes are logged (`sound` category). |
| `20011` | W | Alarm companion | Stored. |
| `20012-20014` | | Keyboard interface, see below | Observed. |
| `20015` | W | Bit 1 = speaker gate (click/beep codes); bit 0 set at end of POST; bits 6, 3, 2 = Twinax wrap/loop-back mode | Hypothesis. Wrap mode not modelled. |
| `20020-2002F` | R/W | Twinax engine: transmit DMA address (`20020`) and count (`20022`), frame/pattern registers (`20024-28`), receiver data/status (`2002B`) | Hypothesis. Plain storage. |
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

## Boot progress with real devices (no forced results)

Observed with `--display green`, an ephemeral EEPROM and no Twinax device:

1. Reset, register-only RAM test, ROM checksums, strap decoding.
2. POST tests: RAM passes, then the communications loop-back test (error
   `COM`) fails and POST stops with the alarm tone on (`20010` = `9B`) and
   the CPU halted. This is expected until the Twinax engine is modelled.

What the `COM` test needs from the Twinax engine (hypothesis from the
register traffic): with wrap mode selected in `20015`, a transmit DMA of a
short buffer (address `20020`, count `20022`) looped back to the receiver,
which writes the received bytes to RAM through the channel's receive
descriptor and updates the descriptor's status and count; the last byte is
also visible in `2002B`. A link device that implements the wrap path at
`20000-2002F` (hook: `Ibm3477TwinaxPort`) should get POST past `COM`.

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

1. Twinax engine: the wrap/loop-back path for POST, then the link to
   `TwinaxHost` (a polling controller is enough for the boot screens).
2. Monitor sense nibble in `2012F` for the display test.
3. Confirm the strap/display-type mapping and the CPU type and clock on a
   board.
