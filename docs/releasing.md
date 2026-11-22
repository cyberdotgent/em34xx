# Releasing

The version is `project(em34xx VERSION x.y.z)` in `CMakeLists.txt`. Builds
show it as `x.y` (or `x.y.z` when z is not 0) followed by the git short hash
of the commit they were built from, e.g. `0.1-abcdef1` (`--version`,
Help > About in em34xxw, the Windows version resource). The header is
regenerated at every build (`cmake/version.cmake`).

## CI (.github/workflows/ci.yml)

Every push and pull request:

- **Linux**: debug build and `ctest -LE slow` (no ROMs on CI, so the ROM
  smoke test is not registered there).
- **Windows x64**: MinGW cross-build (`mingw-x64-release` preset), packaged
  as `em34xx-<version>-win64.zip` (em34xxw.exe, em34xx-headless.exe,
  LICENSE, README.md) and uploaded as a workflow artifact, downloadable from
  the run's page.

A tag `v*` additionally creates a GitHub release named after the tag with
the Windows zip attached. The tag must match the project version (`v0.2`
for 0.2.0), otherwise the release job fails. The released zip is the
end-user client only: the release job removes em34xx-headless.exe (the
developer tool with the monitor and the GDB stub), which stays in the
on-commit workflow artifact.

The release also carries a `SHA256SUMS` file listing the SHA-256 of every
other attached asset; after downloading, check with `sha256sum -c
SHA256SUMS` (on Windows: `certutil -hashfile <zip> SHA256` and compare).
The checksums detect a corrupted or swapped download; the binaries are not
code-signed.

## Hardening

The project's own targets are built with exploit mitigations
(`EM34XX_HARDENING`, on by default, in `CMakeLists.txt`): stack protector
(`-fstack-protector-strong`) and, in optimised builds, `_FORTIFY_SOURCE=2`;
on Linux full RELRO (`-z relro -z now`); on Windows ASLR with base
relocations kept (`--dynamicbase --enable-reloc-section`), DEP
(`--nxcompat`) and, on x64, high-entropy ASLR (`--high-entropy-va`). The
stack-protector runtime is linked statically, so the executables still
import system DLLs only (it adds `ADVAPI32.dll` to em34xx-headless.exe).
To check a build: `x86_64-w64-mingw32-objdump -p em34xxw.exe` shows the
`DllCharacteristics` flags and the `DLL Name` imports.

## Cutting a release ("cut release v0.2")

1. Set `project(em34xx VERSION 0.2.0)` in `CMakeLists.txt`.
2. Run the tests locally: `cmake --build --preset debug && ctest --preset
   debug -LE slow` (and the ROM smoke test, with the ROMs present), plus a
   `mingw-x64-release` build of `em34xxw`.
3. Commit the version bump ("Release v0.2"), tag it `v0.2` (annotated), and
   push the commit and the tag: `git push origin main v0.2`.
4. CI builds, tests and publishes the release.
