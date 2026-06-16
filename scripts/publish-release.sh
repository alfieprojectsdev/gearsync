#!/usr/bin/env bash
# Build the test-drive APK (fusion + audio cues ON) and publish it as a GitHub
# Release asset so testers can download + install from a phone browser — no
# rename-to-PDF juggling, and no binary committed to git.
#
# IDEMPOTENT: if the release/tag already exists the asset is re-uploaded with
# --clobber (replaces in place); otherwise the release is created. Re-running just
# refreshes the attached APK.
#
# Usage: scripts/publish-release.sh <tag> [title]
#   e.g. scripts/publish-release.sh v0.1.0-testdrive "Neighborhood test drive"
set -euo pipefail
cd "$(dirname "$0")/.."

TAG="${1:?usage: scripts/publish-release.sh <tag> [title]}"
TITLE="${2:-GearSync $TAG}"
APK="gearsync-testdrive-fusion.apk"

NOTES=$(cat <<'EOF'
Free, still pretty buggy, but it has potential.

Manual-transmission shift assistant — infers gear and optimal-shift zone from the
phone mic + accelerometer (no OBD-II dongle, no cloud, all on-device). Ships tuned
for a Toyota Wigo 1.0 E M/T; works first-drive on seeds and self-refines.

Install (Android 8+):
  1. Download the .apk below.
  2. When prompted, allow "Install unknown apps" for your browser / file manager.
  3. Open the .apk to install. Grant Microphone + Location when asked, mount the
     phone on the dash (landscape), tap Start, and drive.

This is a debug build for friends-and-neighbours testing. Feedback welcome.
EOF
)

# Build a fresh APK (idempotent helper; reverts the config flags afterward).
"$(dirname "$0")/build-testdrive-apk.sh"

if gh release view "$TAG" >/dev/null 2>&1; then
  echo ">> release $TAG exists — replacing asset"
  gh release upload "$TAG" "$APK" --clobber
else
  echo ">> creating release $TAG"
  gh release create "$TAG" "$APK" --title "$TITLE" --notes "$NOTES"
fi

echo ">> release URL: $(gh release view "$TAG" --json url -q .url 2>/dev/null || echo "$TAG")"
