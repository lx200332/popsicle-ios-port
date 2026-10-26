#!/bin/bash
# Inspect /reserved-memory/splash_region in the merged popsicle tree.
set -e
DTB=/root/build/mu-silicium/Resources/DTBs/popsicle.dtb
echo "=== node exists? ==="
fdtget -l "$DTB" /reserved-memory | tr ' ' '\n' | grep -i splash || echo "no splash node"
echo
echo "=== properties ==="
for p in $(fdtget -p "$DTB" /reserved-memory/splash_region 2>/dev/null); do
  printf '%-12s ' "$p"
  fdtget -t x "$DTB" /reserved-memory/splash_region "$p" 2>/dev/null || fdtget "$DTB" /reserved-memory/splash_region "$p" 2>/dev/null
done
