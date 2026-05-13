#!/usr/bin/env bash
# release.sh — build firmware, create GitHub release, update the
# `firmware` branch so the device's web /update page can fetch the
# binary directly via raw.githubusercontent.com (CORS-friendly).
#
# Usage: scripts/release.sh vX.Y.Z "Release notes here"
#
# Assumes: platformio.ini already bumped to vX.Y.Z, build is current,
# you are on main with everything committed.

set -euo pipefail

if [ $# -lt 1 ]; then
  echo "usage: $0 vX.Y.Z [release-notes]" >&2
  exit 1
fi

TAG="$1"
NOTES="${2:-Release ${TAG}}"
REPO_ROOT="$(git rev-parse --show-toplevel)"
FIRMWARE_BIN="${REPO_ROOT}/.pio/build/default/firmware.bin"

if [ ! -f "$FIRMWARE_BIN" ]; then
  echo "no firmware.bin at $FIRMWARE_BIN — run 'pio run -e default' first" >&2
  exit 1
fi

echo "==> Creating GitHub release $TAG"
gh release create "$TAG" "$FIRMWARE_BIN" --title "$TAG" --notes "$NOTES"

echo "==> Updating firmware branch with new binary"
WORKTREE="/tmp/firmware-branch-$$"
git worktree add "$WORKTREE" firmware
trap "git worktree remove --force '$WORKTREE' 2>/dev/null || true" EXIT

cp "$FIRMWARE_BIN" "$WORKTREE/firmware.bin"
cd "$WORKTREE"
git add firmware.bin
git commit -m "firmware $TAG"
git push origin firmware
cd "$REPO_ROOT"

echo "==> Purging jsDelivr / GitHub raw caches (best-effort)"
curl -sf "https://purge.jsdelivr.net/gh/${GITHUB_REPOSITORY:-diogo7dias/crosspoint-reader-DX34}@firmware/firmware.bin" >/dev/null || true

echo "==> Done. Release $TAG live, firmware branch updated."
