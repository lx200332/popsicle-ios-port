#!/bin/bash
# defconfig + bootimg: let the UniXNU driver pack the popsicle boot image itself.
set -e
DEV=<BUILD>/UniXNU-popsicle
UEFI=<BUILD>/mu-silicium
UNIXNU=<BUILD>/unixnu

echo "=== dtc present? ==="
which fdtput fdtget dtc || (apt-get install -y device-tree-compiler >/dev/null 2>&1 && echo "installed" && which fdtput fdtget dtc)

echo "=== defconfig ==="
cd "$UNIXNU"
python3 unixnu defconfig popsicle_defconfig -D "$DEV" --uefi "$UEFI" 2>&1 | tail -12 || true

echo "=== bootimg ==="
python3 unixnu bootimg -D "$DEV" --uefi "$UEFI" \
  --fd "$UEFI/Build/popsiclePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd" \
  --dtb "$UEFI/Resources/DTBs/popsicle.dtb" \
  --ramdisk none 2>&1 | tail -30 || true

echo "=== output ==="
find <BUILD>/unixnu/out/popsicle -maxdepth 3 -type f -printf '%s\t%p\n' 2>/dev/null | sort -k2
