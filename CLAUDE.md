# em34xx — notes for Claude

- Pull requests, opened or reviewed, follow `CONTRIBUTING.md` (scope, front-end
  validation, AI disclosure with model and provider).
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
  On macOS `macos-release` builds universal (arm64 + x86_64), em34xx.app
  included (docs/frontend-cocoa.md).
- Tests: `ctest --preset debug -LE slow` (fast); the CPU vectors need
  `tools/fetch-test-vectors.sh` and take about 3 minutes in release.
- The 3477 is the primary bring-up target; its colour variant `3477-FC`
  runs its own ROM pair (`65f2995.bin`, `65f2996.bin`; docs/models.md). The 3486 model runs the 3487's
  complete image (`54g0010.bin`), also as model 3487, and is in bring-up:
  it passes POST, saves its setup, goes on line and shows a TN5250 host's
  screen; see docs/bringup-3486.md for what is untested.
