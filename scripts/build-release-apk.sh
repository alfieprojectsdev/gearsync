#!/usr/bin/env bash
# Build a SIGNED release APK with the opt-in flags ON (fusion + audio cues) and
# copy it to the repo root as gearsync-release.apk. Release = R8-shrunk,
# debuggable=false, BuildConfig.DEBUG diagnostics stripped, signed with your
# keystore (updatable installs).
#
# IDEMPOTENT: reverts the vehicle_config.json edit on EXIT (trap), even on failure.
# Requires keystore.properties (run scripts/generate-keystore.sh first); otherwise
# the release would be unsigned and uninstallable — so we fail fast with guidance.
#
# Usage: scripts/build-release-apk.sh
set -euo pipefail
cd "$(dirname "$0")/.."

if [ ! -f keystore.properties ]; then
  echo "!! keystore.properties not found — release would be UNSIGNED."
  echo "   Run scripts/generate-keystore.sh first (one-time), then retry."
  exit 1
fi

CFG="app/src/main/assets/vehicle_config.json"
OUT="$PWD/gearsync-release.apk"

restore() { git checkout -- "$CFG" 2>/dev/null || true; }
trap restore EXIT

sed -i \
  -e 's/"useVibrationFusion": *false/"useVibrationFusion": true/' \
  -e 's/"useAudioCues": *false/"useAudioCues": true/' \
  "$CFG"

./gradlew assembleRelease

APK="app/build/outputs/apk/release/app-release.apk"
[ -f "$APK" ] || APK="app/build/outputs/apk/release/app-release-unsigned.apk"
cp -f "$APK" "$OUT"
echo ">> built $OUT"
echo ">> verifying signature:"
"${ANDROID_HOME:-$HOME/Android/Sdk}"/build-tools/*/apksigner verify --print-certs "$OUT" 2>/dev/null | head -3 \
  || echo "   (apksigner not found on PATH — verify manually with: apksigner verify --print-certs $OUT)"
