# tn34xx — notes for Claude

- C++17 only (Windows 7 x86 must stay a viable target). vcpkg manifest
  dependencies; replxx, not GNU readline; MinGW-w64 later for Windows.
- Clean room: no code or data from ROM images in `src/`, and no ROM or
  overlay patches. Emulate hardware instead. ROM images live in `roms/`, are
  git-ignored by name, and are documented in `roms/ROMS.md`. Analysis notes
  go in `docs/`; do not commit disassembly listings or byte dumps.
- Other emulators and clients (Solstice, tn5250/lib5250, etc.) are
  behavioural references only, whatever their licence: describe their
  behaviour in analysis notes/specs and reimplement it; never lift or port
  their code.
- Model facts must be labelled observed, documented or hypothesis
  (`ModelInfo::notes`, `docs/bringup-*.md`).
- Commit after every change; push when a remote exists.
- Build: `VCPKG_ROOT=~/vcpkg cmake --preset debug && cmake --build --preset debug`.
- Tests: `ctest --preset debug -LE slow` (fast); the CPU vectors need
  `tools/fetch-test-vectors.sh` and take about 3 minutes in release.
- The 3477 is the primary bring-up target; the 3486 waits for a re-dump.
