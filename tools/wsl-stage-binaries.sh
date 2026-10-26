#!/bin/bash
# Stage Binaries/popsicle from infiniti (same Kaanapali SoC package) and verify all references resolve.
set -e
cd <BUILD>/mu-silicium || exit 1
SRC=<BUILD>/devbin/infiniti
DST=Binaries/popsicle

rm -rf "$DST"
mkdir -p Binaries
cp -a "$SRC" "$DST"
echo "=== copied: $(find "$DST" -type f | wc -l) files, $(du -sh "$DST" | cut -f1) ==="

# collect referenced paths from the three .inc files
grep -o 'Binaries/popsicle/[^ ]*' Platforms/Xiaomi/popsiclePkg/Include/APRIORI.inc \
  Platforms/Xiaomi/popsiclePkg/Include/DXE.inc \
  Platforms/Xiaomi/popsiclePkg/Include/RAW.inc 2>/dev/null | sed 's/^[^:]*://' | sort -u > /tmp/refs.txt
echo "=== referenced unique paths: $(wc -l < /tmp/refs.txt) ==="
missing=0
while read -r p; do
  if [ ! -f "$p" ]; then echo "MISSING: $p"; missing=$((missing+1)); fi
done < /tmp/refs.txt
echo "=== missing: $missing ==="
