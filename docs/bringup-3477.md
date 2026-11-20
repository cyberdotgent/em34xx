# 3477 bring-up log

Hardware facts and hypotheses for the 3477, as worked out from running the
firmware. Each entry is labelled **observed** (seen in emulation or on
hardware), **documented**, or **hypothesis**.

## Memory map

| Range | Contents | Status |
| --- | --- | --- |
| `00000-0FFFF` | RAM (64 KiB mapped) | Hypothesis. The reset code uses a stack at `05D0:xxxx` and RAM-tests `0000:0AA0` and `0000:7500-7FFF`. True size unknown. |
| `20000-2xxxx` | Memory-mapped peripheral | Observed. First device access after reset is a write to `2000:0004`. |
| `C0000-DFFFF` | ROM `95F4172` | Observed (pointer tables). |
| `E0000-FFFFF` | ROM `95F4171`, reset vector | Observed. |

## CPU

- **Hypothesis: 8088-class.** Both ROMs are byte-wide linear images, which
  implies an 8-bit data bus. No 80186 instruction encodings have been
  confirmed yet.
- **Observed: no port I/O on the reset path.** Neither ROM contains
  `MOV DX,imm` followed by `IN`/`OUT DX`. Peripherals appear to be reached
  through far pointers.

## Board

- **Observed (owner, 2026-11-20):** the link-side gate array is a Toshiba
  TC-family part, with SRAMs and line drivers similar to those on IBM's ISA
  5250 emulation adapters (whose Enhanced adapter uses a Toshiba TC110G26AF
  gate array with a byte-wide SRAM and a 16-pin line-interface part). This
  supports the hypothesis that the terminal's Twinax engine is the same
  engine design wired directly onto the CPU bus.

## Next steps

1. Identify the device at segment `2000`: map a probe
   (`map probe 20000 100`) and record its access pattern.
2. Find the display controller and character generator; replace the
   placeholder display.
3. Find the keyboard interface.
4. Find the Twinax link device and wire it to `TwinaxHost`.
5. Confirm the CPU type and clock from the board (photos or chip list).
