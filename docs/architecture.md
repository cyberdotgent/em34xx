# Architecture

```
            SDL3 window (main thread)        monitor (replxx thread / rc / stdin)
                  |  render, keys                  |          ^
                  v                                v          | qRcmd
              +-------------------- Emulator --------------------+   GDB stub
              |  run control, pacing, stop events, machine lock |<-- (thread)
              +--------------------------+----------------------+
                                         | emulation thread, 20k-instruction slices
                                         v
            Machine: I86 CPU  <->  Bus  <->  devices (model-specific)
                                    |             |
                              RAM / ROM      Twinax link device  <->  TwinaxHost
                                                                     (none / script / tn5250)
```

## Principles

- **Models describe hardware, not firmware.** A model maps memory and
  devices. Every fact it encodes is either observed (board, datasheet,
  documentation) or marked as a hypothesis in `ModelInfo::notes`.
- **No ROM patches, no ROM-derived code.** Firmware runs unmodified; the
  emulator contains nothing copied from it.
- **Fail loudly on the unknown.** Accesses the model does not decode stop the
  machine by default, so each unknown piece of hardware becomes a concrete
  task. The `unmapped` report aggregates them; probe devices stand in while
  the behaviour is worked out.
- **One CPU core, several levels.** `CpuLevel` selects 8086/8088 quirks,
  80186 instructions, and (later) 80286 protected mode.

## CPU

`src/cpu/i86.cpp` is an instruction-level interpreter. It is validated
against SingleStepTests 8088 V2. Cycle counts are approximate: deterministic,
and good enough for pacing and timers, but they do not model the prefetch
queue. A REP string instruction is restartable: if an interrupt is pending
between iterations, IP rewinds to the prefix.

## Bus

`Bus` decodes memory in 256-byte pages. The CPU uses byte accesses; word
accesses are split by the CPU according to its bus width. Debugger access
(`peek8`/`poke8`) has no side effects and ignores policy and watchpoints.
The monitor can write RAM only, never ROM.

## Threads

The machine itself is single-threaded. The Emulator runs it in slices on its
own thread and releases the machine lock between slices. The monitor, GDB
stub and front end take the lock briefly through `Emulator::with_machine`.

## Twinax / TN5250

`src/host/twinax_link.hpp` fixes the unit both sides exchange: the 16-bit
5250 workstation frame (sync, data LSB-first, 3-bit station address with 7 as
end-of-message, even parity, fill). The model's link device, still unknown,
will turn firmware register accesses into frames. A `TwinaxHost` plays the
controller. The TN5250 client (`src/tn5250`, see `docs/tn5250.md`) keeps
the 5250 shadow screen and does the operator field editing; the virtual
workstation controller (`src/host/wsc`) drives the station from it through
`tn5250::DisplaySession`.
