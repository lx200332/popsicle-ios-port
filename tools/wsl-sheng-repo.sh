#!/bin/bash
R=/root/build/unixnu-sheng
echo "=== board.toml ==="
cat "$R/board.toml"
echo
echo "=== configs/ ==="
ls -la "$R/configs/"
echo
echo "=== .gitmodules ==="
cat "$R/.gitmodules"
echo
echo "=== unixnu.lock ==="
cat "$R/unixnu.lock"
echo
echo "=== drivers/ ==="
find "$R/drivers" -maxdepth 3 | head -40
echo
echo "=== kits/ ==="
find "$R/kits" -maxdepth 3 | head -20
echo
echo "=== patches/ ==="
find "$R/patches" -maxdepth 2 | head -20
echo
echo "=== package/ ==="
find "$R/package" -maxdepth 3 | head -30
echo
echo "=== scripts (count + names) ==="
ls "$R/scripts" | head -60
echo "... total: $(ls "$R/scripts" | wc -l)"
