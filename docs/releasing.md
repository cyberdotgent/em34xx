# Releasing

The version is `project(tn34xx VERSION x.y.z)` in `CMakeLists.txt`. Builds
show it as `x.y` (or `x.y.z` when z is not 0) followed by the git short hash
of the commit they were built from, e.g. `0.1-abcdef1` (`--version`,
Help > About in tn34xxw, the Windows version resource). The header is
regenerated at every build (`cmake/version.cmake`).

## CI (.github/workflows/ci.yml)

Every push and pull request:

- **Linux**: debug build and `ctest -LE slow` (no ROMs on CI, so the ROM
  smoke test is not registered there).
- **Windows x64**: MinGW cross-build (`mingw-x64-release` preset), packaged
  as `tn34xx-<version>-win64.zip` (tn34xxw.exe, tn34xx-headless.exe,
  LICENSE, README.md) and uploaded as a workflow artifact, downloadable from
  the run's page.

A tag `v*` additionally creates a GitHub release named after the tag with
the Windows zip attached. The tag must match the project version (`v0.2`
for 0.2.0), otherwise the release job fails.

## Cutting a release ("cut release v0.2")

1. Set `project(tn34xx VERSION 0.2.0)` in `CMakeLists.txt`.
2. Run the tests locally: `cmake --build --preset debug && ctest --preset
   debug -LE slow` (and the ROM smoke test, with the ROMs present), plus a
   `mingw-x64-release` build of `tn34xxw`.
3. Commit the version bump ("Release v0.2"), tag it `v0.2` (annotated), and
   push the commit and the tag: `git push origin main v0.2`.
4. CI builds, tests and publishes the release.
