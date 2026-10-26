#!/bin/bash
# Restore the mu-silicium worktree: clear phantom staged deletions, then checkout missing blobs.
set -x
cd /c/dsh-对话/mu-silicium || exit 1
rm -f .git/index.lock

git reset -q 2>&1 | tail -3
echo "after reset:"
git status --short 2>&1 | head -5

# Checkout everything except submodule dirs, in chunks so a proxy drop doesn't lose all progress
echo "=== restoring worktree ==="
git -c core.longpaths=true checkout -- . 2>&1 | tail -5

echo "=== counts ==="
git ls-tree -r --name-only HEAD | wc -l
find . -path ./.git -prune -o -type f -print | wc -l
echo "=== remaining deleted ==="
git status --short 2>&1 | grep '^ D' | head -10
echo "=== DONE ==="
