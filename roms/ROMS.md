# ROM images

ROM images are **not** part of this repository. Place them in this directory
under the names below; they are listed in `.gitignore`. The emulator verifies
them by SHA-256 and contains no code or data taken from them.

| Model | File | Size | SHA-256 | Status |
| --- | --- | ---: | --- | --- |
| 3486 | `54g0011.bin` | 524288 | `f5d968694fdb3b6991b219460eb0ce5eec131797a9c0ea928ec61a6a5b9291ef` | **Incomplete — half missing.** Read as a 27C040. See analysis below. Needs re-dump. |
| 3477 | `95f4171.bin` | 131072 | `3f7fb58aec7505315ff94206c8dc05102e319944bd0bde55092274777fb39fde` | **Good (primary bring-up target).** Maps at physical `0xE0000..0xFFFFF`; holds the reset vector. No invariant address bit. |
| 3477 | `95f4172.bin` | 131072 | `290a58cd5db6543957bdcb2c9e92efefd62a54e37bcd239d41d9bd9cdd6be1d0` | **Good.** Maps at physical `0xC0000..0xDFFFF`. No invariant address bit. |

## Known-bad dumps

These hashes identify dumps that must not be used for bring-up. The emulator
refuses them unless explicitly overridden.

| Model | File | SHA-256 | Problem |
| --- | --- | --- | --- |
| 3486 | `54g0011.bin` | `f5d968694fdb3b6991b219460eb0ce5eec131797a9c0ea928ec61a6a5b9291ef` | Chip A17 stuck high during the read; segments `8000`, `9000`, `C000` and `D000` are missing (see below). Re-dump requested. |

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
