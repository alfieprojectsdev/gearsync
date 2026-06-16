#!/usr/bin/env bash
# Land a review-approved PR branch end to end — IDEMPOTENT (safe to re-run; every
# step no-ops if already done):
#   1. merge on GitHub with --merge (NO --delete-branch, which triggers the
#      "fatal: 'main' is already used by worktree" local-cleanup error when main
#      is checked out in the root worktree — the remote merge still succeeds, but
#      we avoid the noise entirely),
#   2. remove the worktree checked out on the branch (if any),
#   3. delete the local + remote branch (if present),
#   4. fast-forward local main.
#
# Usage: scripts/land-pr.sh <branch>
set -euo pipefail
cd "$(dirname "$0")/.."

BRANCH="${1:?usage: scripts/land-pr.sh <branch>}"

# 1. Merge only if a PR for this branch is still OPEN.
state="$(gh pr view "$BRANCH" --json state -q .state 2>/dev/null || echo NONE)"
case "$state" in
  OPEN)   echo ">> merging $BRANCH"; gh pr merge "$BRANCH" --merge ;;
  MERGED) echo ">> PR for $BRANCH already MERGED — skipping merge" ;;
  NONE)   echo ">> no PR found for $BRANCH (already cleaned up?) — continuing" ;;
  *)      echo ">> PR state for $BRANCH is '$state' — not merging" ;;
esac

# 2. Remove the worktree on this branch, if one exists.
wt="$(git worktree list --porcelain | awk -v b="refs/heads/$BRANCH" '
  /^worktree /{p=substr($0,10)} /^branch /{ if (substr($0,8)==b) print p }')"
if [ -n "${wt:-}" ] && [ -d "$wt" ]; then
  echo ">> removing worktree $wt"
  git worktree remove --force "$wt"
fi
git worktree prune

# 3. Delete local + remote branch (ignore if already gone).
git branch -D "$BRANCH" 2>/dev/null && echo ">> deleted local $BRANCH"  || true
git push origin --delete "$BRANCH" 2>/dev/null && echo ">> deleted remote $BRANCH" || true

# 4. Fast-forward main (no-op if already current).
echo ">> fast-forwarding main"
git fetch origin -q
git switch main 2>/dev/null || git checkout main 2>/dev/null || true
git merge --ff-only origin/main
echo ">> main now: $(git log --oneline -1)"
