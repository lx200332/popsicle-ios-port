#!/bin/bash
set -u
R=<BUILD>/unixnu
cd "$R" || exit 1
echo "=== unixnu wrapper ==="
cat ./unixnu
echo
echo "=== scripts/unixnu/ ==="
ls -la ./scripts/unixnu/ 2>/dev/null
echo
echo "=== scripts subdirs ==="
ls -d ./scripts/*/ 2>/dev/null
echo
echo "=== platform checks ==="
grep -rn "sys.platform" ./scripts/unixnu/ 2>/dev/null | head -20
echo
echo "=== board.toml refs ==="
grep -rn "board.toml" ./scripts/unixnu/ 2>/dev/null | head -10
echo
echo "=== python ==="
python3 -V
echo
echo "=== help ==="
if head -1 ./unixnu | grep -q python; then
  python3 ./unixnu --help 2>&1 | head -60
else
  bash ./unixnu --help 2>&1 | head -60
fi
