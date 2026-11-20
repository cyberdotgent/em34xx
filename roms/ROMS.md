# ROM images

ROM images are **not** part of this repository. Place them in this directory
under the names below; they are listed in `.gitignore`. The emulator verifies
them by SHA-256 and contains no code or data taken from them.

| Model | File | Size | SHA-256 | Status |
| --- | --- | ---: | --- | --- |
| 3486 | `54g0011.bin` | 524288 | `f5d968694fdb3b6991b219460eb0ce5eec131797a9c0ea928ec61a6a5b9291ef` | **Complete chip, incomplete firmware.** The chip is a 256 KiB mask ROM and this file holds all of it (twice, as a 27C040 read). The firmware also needs segments 8000/9000/C000/D000 from another source (see below). |
| 3477 | `95f4171.bin` | 131072 | `3f7fb58aec7505315ff94206c8dc05102e319944bd0bde55092274777fb39fde` | **Good (primary bring-up target).** Maps at physical `0xE0000..0xFFFFF`; holds the reset vector. No invariant address bit. |
| 3477 | `95f4172.bin` | 131072 | `290a58cd5db6543957bdcb2c9e92efefd62a54e37bcd239d41d9bd9cdd6be1d0` | **Good.** Maps at physical `0xC0000..0xDFFFF`. No invariant address bit. |

## Known-bad dumps

These hashes identify dumps that must not be used for bring-up. The emulator
refuses them unless explicitly overridden.

| Model | File | SHA-256 | Problem |
| --- | --- | --- | --- |
| 3486 | `54g0011.bin` | `f5d968694fdb3b6991b219460eb0ce5eec131797a9c0ea928ec61a6a5b9291ef` | Not a bad read, but not runnable on its own: segments `8000`, `9000`, `C000` and `D000`, which the firmware calls and checksums, are not in this chip (see below). |

## 3477 placement

Each chip is a byte-wide, linearly ordered image (not an even/odd pair).
Self-referencing far-pointer tables place `95f4172` in segments `C000`/`D000`
and the second half of `95f4171` in `F000`; the reset vector at the top of
`95f4171` jumps to `F000:0000`. Two byte-wide ROMs with contiguous code suggest
an 8-bit CPU bus (8088-class) — a hypothesis to confirm from the board. Neither
image contains any `IN`/`OUT DX` sequences, so the hardware is most likely
memory-mapped.

Verify with:

```sh
sha256sum roms/*.bin
```

## 3486 `54g0011.bin` analysis

The friend read the sole 3486 ROM as a 27C040 (512 KiB). The file does not
repeat at a size boundary (no bit-18 invariance), so the device is not a
smaller part read with too many address bits. Instead **file-address bit 17
(27C040 pin 30) is exactly invariant** while bit 18 (pin 31) is not:

```
64 KiB file blocks:  0  1  2  3  4  5  6  7
content:             A  B  A  B  C  D  C  D
```

Far-pointer tables inside each block reveal the segment each block was
linked for (a table whose entries point at the table itself or its
neighbouring data):

| File block | Self-referencing segment |
| --- | --- |
| 0 / 2 | `A000` |
| 1 / 3 | `B000` |
| 4 / 6 | `E000` |
| 5 / 7 | `F000` (also holds the reset vector) |

So the ROM is a 512 KiB part mapped at physical `0x80000..0xFFFFF`
(file offset = physical − `0x80000`), and during the read the chip's A17 sat
**high** constantly. We have segments `A000`, `B000`, `E000`, `F000`; the
`8000`, `9000`, `C000` and `D000` blocks are **missing**, even though the
present code holds pointers and far calls into all four of them.

The pattern (pin 30 ignored, pin 31 is a real address line, A17 constantly
high) fits a mask ROM with A17 on **pin 1**, where a 27C040 has Vpp. The
programmer holds Vpp high while reading. This is a hypothesis to check
against the chip's datasheet or marking.

Re-dump options, in order of preference:

1. Lift pin 1 and jumper it to socket pin 30, then read as 27C040. That
   should give the complete, correctly ordered image in one read.
2. Lift pin 1, tie it to GND, and read as 27C040. File blocks 0/1/4/5 would
   then hold the missing `8000/9000/C000/D000` segments, to be merged with
   the current read.

## 3486 `54g0011` re-examined (second dumping session)

Later reads settled the chip itself. Every one of its 32 pins is accounted
for: pin 30 has no effect, pin 31 is an address line (CPU A18), and pins 22
and 24 are enables (lifting either reads all 00 on a reader with
pull-downs). A 27C080 read (1 MiB) is the 512 KiB file twice
(md5 `9e91319c67cdb873a02aff30465c0637`). So the chip is a **256 KiB mask
ROM, completely dumped**: the earlier "A17 stuck high, half missing"
reading of the file was wrong. On the board it is selected only when CPU
A17 = 1, appearing at `A0000-BFFFF` and `E0000-FFFFF`.

The firmware nevertheless calls into `D000`, keeps pointer tables into
`9000`, and its POST checksum covers `8000-FFFF` (target `619A`; this chip
sums to `DBE9`, so the rest must sum to `85B1`). Where those four segments
come from is not settled. The firmware treats them as part of one linked
512 KiB image: it has no loader, decompressor or download path into
`80000-DFFFF` and no presence check, and the referenced data alone is larger
than all the board's SRAM, so battery-backed RAM is ruled out. The leading
hypothesis is a second 256 KiB ROM on a card in J5 (a 50-position
expansion connector), decoded for CPU A17 = 0. That conflicts with the
owner's understanding that J5 was never used and still needs confirming
(see `docs/bringup-3486.md`, "Where the missing half comes from").
