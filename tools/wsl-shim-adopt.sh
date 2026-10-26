#!/bin/bash
# Adopt the uefi-sheng BootShim.S in the Mu-Silicium checkout, rebuild, then let unixnu pack the image.
set -e
UEFI=/root/build/mu-silicium
DEV=/root/build/UniXNU-popsicle
UNIXNU=/root/build/unixnu

cp "$UEFI/BootShim/BootShim.S" "$UEFI/BootShim/BootShim.S.musilicium"
cp /root/build/uefi-sheng/BootShim/BootShim.S "$UEFI/BootShim/BootShim.S"
echo "=== adopted uefi-sheng BootShim.S ==="
grep -n 'Image Size\|FD_SIZE +\|dsb\|ic ' "$UEFI/BootShim/BootShim.S" | head

echo "=== rebuild firmware + image ==="
cd "$UEFI"
python3 build_uefi.py -d popsicle 2>&1 | tail -4
ls -l Build/popsiclePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd Mu-popsicle.img BootShim/BootShim.bin

echo "=== unixnu bootimg ==="
cd "$UNIXNU"
python3 unixnu bootimg -D "$DEV" --uefi "$UEFI" \
  --fd "$UEFI/Build/popsiclePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd" \
  --dtb "$UEFI/Resources/DTBs/popsicle.dtb" \
  --ramdisk none 2>&1 | tail -20 || true

echo "=== output ==="
find /root/build/unixnu/out/popsicle -maxdepth 3 -type f -printf '%s\t%p\n' 2>/dev/null | sort -k2
