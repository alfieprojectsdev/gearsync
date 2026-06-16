# scripts/

Reusable, **idempotent** helper scripts (safe to re-run). Run from anywhere — each
`cd`s to the repo root itself.

| Script | What it does |
|---|---|
| `land-pr.sh <branch>` | Merge an open PR (`--merge`, **no** `--delete-branch` — that's what caused the recurring `fatal: 'main' is already used by worktree`), remove its worktree, delete local+remote branch, fast-forward `main`. No-ops anything already done. |
| `build-testdrive-apk.sh` | Build a **debug** APK with the opt-in flags (vibration fusion + audio cues) ON → repo-root `gearsync-testdrive-fusion.apk`. Always reverts the config edit. Debug-signed: fine for one-off testing, but installs can't be *updated* in place. |
| `generate-keystore.sh [path]` | **One-time.** Create the release-signing keystore (default `~/.android/keystores/gearsync-release.jks`, outside the repo) and scaffold `keystore.properties`. Refuses to overwrite an existing keystore. `keytool` prompts for the password — never hardcoded. |
| `build-release-apk.sh` | Build a **signed release** APK (R8-shrunk, `debuggable=false`, DEBUG diagnostics stripped) with fusion+cues ON → repo-root `gearsync-release.apk`. Requires `keystore.properties`; fails fast with guidance if absent. Use for updatable tester builds. |
| `publish-release.sh <tag> [title]` | Build the test-drive (debug) APK and attach it to a GitHub Release (creates it, or re-uploads with `--clobber`). For sharing with testers — APKs are **never** committed to git (see `.gitignore`). |
| `sync-main.sh` | Fetch + fast-forward local `main`. |

**Release signing:** `keystore.properties`, `*.jks`, `*.keystore` are gitignored — NEVER commit them. `keystore.properties.template` (committed) shows the shape. The `.jks` belongs in encrypted cloud + a 2nd backup; the password in a password manager, kept **separate** from the key. Losing both = you can never ship an update that installs over an existing GearSync install.

The other `merge-pr*.sh` / `open-pr-*.sh` files are one-off, per-PR historical scripts.
