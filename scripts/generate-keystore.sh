#!/usr/bin/env bash
# Generate the release-signing keystore (one-time) and scaffold keystore.properties.
# IDEMPOTENT/SAFE: refuses to overwrite an existing keystore (losing it = can never
# update installed apps). keytool prompts for the password interactively — it is
# never passed on the command line or written by this script beyond keystore.properties,
# which is gitignored.
#
# Usage: scripts/generate-keystore.sh [keystore-path]
#   default path: ~/.android/keystores/gearsync-release.jks (outside the repo)
set -euo pipefail
cd "$(dirname "$0")/.."

KS="${1:-$HOME/.android/keystores/gearsync-release.jks}"
ALIAS="gearsync"
PROPS="keystore.properties"

if [ -f "$KS" ]; then
  echo "!! keystore already exists: $KS"
  echo "   Refusing to overwrite (overwriting it permanently breaks updates to installed apps)."
  exit 1
fi

mkdir -p "$(dirname "$KS")"

echo ">> Generating RSA-2048 keystore (valid 10000 days) at: $KS"
echo "   You will be prompted for a keystore password and a distinguished name."
echo "   REMEMBER THE PASSWORD — store it in a password manager, separate from the .jks."
keytool -genkeypair \
  -keystore "$KS" \
  -alias "$ALIAS" \
  -keyalg RSA -keysize 2048 \
  -validity 10000

# Scaffold keystore.properties (gitignored). Passwords are NOT auto-filled — edit it.
if [ -f "$PROPS" ]; then
  echo ">> $PROPS already exists — leaving it untouched."
else
  cat > "$PROPS" <<EOF
storeFile=$KS
storePassword=CHANGE_ME
keyAlias=$ALIAS
keyPassword=CHANGE_ME
EOF
  echo ">> wrote $PROPS (gitignored) — set storePassword/keyPassword to the password you just chose."
fi

cat <<EOF

>> Done. Next:
   1. Edit $PROPS — set storePassword and keyPassword (same password unless you set a key password).
   2. Back up $KS to encrypted cloud + a 2nd location; store the password in a password manager (SEPARATELY).
   3. Build a signed release:  scripts/build-release-apk.sh
EOF
