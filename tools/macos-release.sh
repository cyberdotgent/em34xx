#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build, sign and attach the macOS release of em34xx (docs/releasing.md).
#
# Run on a Mac, from a clean checkout of the release tag, after CI has
# published the GitHub release with the Windows build:
#
#   tools/macos-release.sh v0.3 --identity "Developer ID Application: ..." \
#       [--notary-profile PROFILE] [--dry-run]
#
#   --identity        codesign identity (also EM34XX_MAC_SIGN_IDENTITY); "-"
#                     signs ad hoc, which is only accepted with --dry-run
#   --notary-profile  a `xcrun notarytool store-credentials` keychain
#                     profile (also EM34XX_NOTARY_PROFILE): the zip is
#                     notarised and the ticket stapled to the app
#   --dry-run         build, sign and package into dist/, upload nothing
#
# Steps: check the tag, the tree and the version; build the macos-release
# preset (universal arm64 + x86_64) in build/macos-dist; check both
# architectures; sign the app with the hardened runtime; verify; zip with
# ditto as em34xx-<version>-macos.zip (the end-user app only, as the
# Windows release has no em34xx-headless); notarise; then upload the zip to
# the release and add its line to the release's SHA256SUMS.
set -euo pipefail

usage() { sed -n '3,25p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

tag=""
identity="${EM34XX_MAC_SIGN_IDENTITY:-}"
notary="${EM34XX_NOTARY_PROFILE:-}"
dry_run=0
while [ $# -gt 0 ]; do
  case "$1" in
    --identity) identity="$2"; shift 2 ;;
    --notary-profile) notary="$2"; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
    -h|--help) usage ;;
    v*) tag="$1"; shift ;;
    *) echo "unknown argument: $1" >&2; usage ;;
  esac
done
[ -n "$tag" ] || usage
die() { echo "macos-release: $*" >&2; exit 1; }

cd "$(dirname "$0")/.."
[ "$(uname)" = Darwin ] || die "run this on macOS"
[ -n "$identity" ] || die "no signing identity (--identity or EM34XX_MAC_SIGN_IDENTITY)"
if [ "$identity" = "-" ] && [ $dry_run -eq 0 ]; then
  die "an ad hoc signature (-) is for --dry-run only"
fi
: "${VCPKG_ROOT:=$HOME/vcpkg}"
export VCPKG_ROOT
[ -x "$VCPKG_ROOT/vcpkg" ] || die "vcpkg not found at $VCPKG_ROOT (set VCPKG_ROOT)"
[ $dry_run -eq 1 ] || command -v gh >/dev/null || die "the GitHub CLI (gh) is needed to upload"

# The checkout is the tag, unmodified, and the tag is the project version.
[ -z "$(git status --porcelain --untracked-files=no)" ] || die "the working tree has changes"
head=$(git rev-parse HEAD)
tagged=$(git rev-parse "$tag^{commit}" 2>/dev/null) || die "no tag $tag"
[ "$head" = "$tagged" ] || die "HEAD is not $tag (git checkout $tag)"
project=$(sed -n 's/^  VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -1)
base=${tag#v}
case "$project" in "$base"|"$base.0"|"$base.0.0") ;; *) die "tag $tag does not match project version $project" ;; esac
if [ $dry_run -eq 0 ]; then
  gh release view "$tag" >/dev/null 2>&1 || die "no GitHub release $tag yet (wait for CI)"
fi

# A build folder of its own, from scratch.
preset=macos-release
build=build/macos-dist
rm -rf "$build"
cmake --preset "$preset" -B "$build" -DEM34XX_BUILD_TESTS=ON
cmake --build "$build"
ctest --test-dir "$build" -LE slow --output-on-failure

app="$build/em34xx.app"
exe="$app/Contents/MacOS/em34xx"
archs=$(lipo -archs "$exe")
case "$archs" in *arm64*x86_64*|*x86_64*arm64*) ;; *) die "$exe is not universal ($archs)" ;; esac
version=$(sed -n 's/^#define EM34XX_VERSION_STRING "\(.*\)"$/\1/p' "$build/generated/em34xx_version.h")
name="em34xx-${version}-macos"

# Sign: hardened runtime and a secure timestamp for a Developer ID.
dist=dist
rm -rf "$dist/$name" "$dist/$name.zip"
mkdir -p "$dist/$name"
ditto "$app" "$dist/$name/em34xx.app"
cp LICENSE README.md "$dist/$name/"
if [ "$identity" = "-" ]; then
  codesign --force --sign - "$dist/$name/em34xx.app"
else
  codesign --force --options runtime --timestamp --sign "$identity" "$dist/$name/em34xx.app"
fi
codesign --verify --strict --verbose=2 "$dist/$name/em34xx.app"
(cd "$dist" && ditto -c -k --keepParent "$name" "$name.zip")

if [ -n "$notary" ]; then
  xcrun notarytool submit "$dist/$name.zip" --keychain-profile "$notary" --wait
  xcrun stapler staple "$dist/$name/em34xx.app"
  spctl --assess --type execute --verbose "$dist/$name/em34xx.app"
  rm -f "$dist/$name.zip"
  (cd "$dist" && ditto -c -k --keepParent "$name" "$name.zip")
fi

echo "built $dist/$name.zip ($archs)"
shasum -a 256 "$dist/$name.zip"
if [ $dry_run -eq 1 ]; then
  echo "dry run: nothing uploaded"
  exit 0
fi

# Upload, then add the zip to the release's SHA256SUMS (same format as
# sha256sum: "<hash>  <file>"), replacing an earlier line for it.
gh release upload "$tag" "$dist/$name.zip" --clobber
sums=$(mktemp -d)
trap 'rm -rf "$sums"' EXIT
gh release download "$tag" --pattern SHA256SUMS --dir "$sums"
grep -v "  $name.zip\$" "$sums/SHA256SUMS" > "$sums/SHA256SUMS.new" || true
(cd "$dist" && shasum -a 256 "$name.zip") >> "$sums/SHA256SUMS.new"
mv "$sums/SHA256SUMS.new" "$sums/SHA256SUMS"
cat "$sums/SHA256SUMS"
gh release upload "$tag" "$sums/SHA256SUMS" --clobber
echo "attached $name.zip to release $tag"
