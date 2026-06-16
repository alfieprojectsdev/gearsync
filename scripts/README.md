# scripts/

Reusable, **idempotent** helper scripts (safe to re-run). Run from anywhere — each
`cd`s to the repo root itself.

| Script | What it does |
|---|---|
| `land-pr.sh <branch>` | Merge an open PR (`--merge`, **no** `--delete-branch` — that's what caused the recurring `fatal: 'main' is already used by worktree`), remove its worktree, delete local+remote branch, fast-forward `main`. No-ops anything already done. |
| `build-testdrive-apk.sh` | Build a debug APK with the opt-in flags (vibration fusion + audio cues) ON → repo-root `gearsync-testdrive-fusion.apk`. Always reverts the config edit. |
| `publish-release.sh <tag> [title]` | Build the test-drive APK and attach it to a GitHub Release (creates the release, or re-uploads the asset with `--clobber` if the tag exists). For sharing with testers — APKs are **never** committed to git (see `.gitignore`). |
| `sync-main.sh` | Fetch + fast-forward local `main`. |

The other `merge-pr*.sh` / `open-pr-*.sh` files are one-off, per-PR historical scripts.
