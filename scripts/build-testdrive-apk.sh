#!/usr/bin/env bash
# Build a debug APK with the opt-in flags ON (ADR 004 vibration fusion + ADR 006
# audio cues) and copy it to the repo root as the test-drive / demo APK.
# IDEMPOTENT: the vehicle_config.json edit is always reverted (trap on EXIT, even
# on build failure), so re-running starts from a clean tree and just rebuilds.
#
# Usage: scripts/build-testdrive-apk.sh
set -euo pipefail
cd "$(dirname "$0")/.."

CFG="app/src/main/assets/vehicle_config.json"
OUT="$PWD/gearsync-testdrive-fusion.apk"

restore() { git checkout -- "$CFG" 2>/dev/null || true; }
trap restore EXIT

# Flip the two opt-in flags on for this build (committed default stays false).
sed -i \
  -e 's/"useVibrationFusion": *false/"useVibrationFusion": true/' \
  -e 's/"useAudioCues": *false/"useAudioCues": true/' \
  "$CFG"

./gradlew assembleDebug
cp -f app/build/outputs/apk/debug/app-debug.apk "$OUT"
echo ">> built $OUT"
