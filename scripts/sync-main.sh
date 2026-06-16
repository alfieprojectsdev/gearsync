#!/usr/bin/env bash
# Fetch origin and fast-forward local main. IDEMPOTENT ("Already up to date" when
# nothing to do). Run after merges if you only want the sync without branch cleanup.
#
# Usage: scripts/sync-main.sh
set -euo pipefail
cd "$(dirname "$0")/.."

git fetch origin -q
git switch main 2>/dev/null || git checkout main 2>/dev/null || true
git merge --ff-only origin/main
echo ">> main now: $(git log --oneline -1)"
