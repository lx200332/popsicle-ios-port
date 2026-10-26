#!/bin/bash
# Sync the hand-built popsiclePkg into the WSL build tree, then verify Binaries references.
set -e
cd /root/build/mu-silicium || exit 1
SRC="/mnt/c/dsh-对话/mu-silicium/Platforms/Xiaomi/popsiclePkg"
DST="Platforms/Xiaomi/popsiclePkg"

rm -rf "$DST"
mkdir -p "$DST"
cp -a "$SRC"/. "$DST"/
echo "=== synced popsiclePkg: $(find "$DST" -type f | wc -l) files ==="
find "$DST" -type f | sed "s|$DST/||" | sort
echo
echo "=== referenced Binaries paths ==="
grep -ho 'Binaries/popsicle/[^ ]*' "$DST"/Include/APRIORI.inc "$DST"/Include/DXE.inc "$DST"/Include/RAW.inc 2>/dev/null | sort -u > /tmp/refs.txt
echo "count: $(wc -l < /tmp/refs.txt)"
missing=0
while read -r p; do
  if [ ! -f "$p" ]; then echo "MISSING: $p"; missing=$((missing+1)); fi
done < /tmp/refs.txt
echo "=== missing: $missing ==="
