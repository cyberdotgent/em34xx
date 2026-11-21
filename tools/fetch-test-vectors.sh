#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch the SingleStepTests CPU test vectors (MIT licensed) into testdata/.
# The data is large (~760 MB for the 8088 set) and is never committed.
#
# usage: tools/fetch-test-vectors.sh [8088] [dest-dir]
set -euo pipefail

SET="${1:-8088}"
DEST="${2:-$(cd "$(dirname "$0")/.." && pwd)/testdata}"

case "$SET" in
  8088)
    REPO="SingleStepTests/8088"
    COMMIT="aea84484abc79d09639d855b7b0ab32bc9e4dbeb"   # 8088 V2 2.0.1
    SUBDIR="v2"
    ;;
  *)
    echo "unknown test set: $SET" >&2
    exit 2
    ;;
esac

OUT="$DEST/$SET"
mkdir -p "$OUT"
echo "$REPO@$COMMIT/$SUBDIR -> $OUT"

LIST="$OUT/.filelist"
if command -v gh >/dev/null 2>&1; then
  gh api "repos/$REPO/contents/$SUBDIR?ref=$COMMIT" --jq '.[].name' > "$LIST"
else
  curl -fsSL "https://api.github.com/repos/$REPO/contents/$SUBDIR?ref=$COMMIT" \
    | grep '"name"' | sed -E 's/.*"name": "([^"]+)".*/\1/' > "$LIST"
fi

BASE="https://raw.githubusercontent.com/$REPO/$COMMIT/$SUBDIR"
export BASE OUT
# Skip files already present; download with limited parallelism.
grep -v '^$' "$LIST" | xargs -P 8 -I{} sh -c '
  f="$OUT/{}"
  [ -s "$f" ] && exit 0
  curl -fsSL --retry 3 -o "$f.part" "$BASE/{}" && mv "$f.part" "$f" || { echo "failed: {}" >&2; exit 1; }
'
echo "$COMMIT" > "$OUT/.commit"
echo "done: $(ls "$OUT" | grep -c json) files"
