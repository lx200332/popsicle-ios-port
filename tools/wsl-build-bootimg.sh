#!/bin/bash
# Rebuild the popsicle UEFI volume and the UniXNU boot image from scratch.
#
# Two arguments the driver cannot infer on this port:
#   --target RELEASE  build_uefi.py emits RELEASE_CLANGPDB, but unixnu defaults to DEBUG
#   --dtb             the assembled tree under build/<board>/ only exists after "unixnu tree",
#                     which needs every feed fetched; point it at the vendor DTB instead
set -e
UEFI=/root/build/mu-silicium
DEV=/root/build/UniXNU-popsicle
UNIXNU=/root/build/unixnu

echo "== 1/3 firmware =="
cd "$UEFI"
python3 build_uefi.py -d popsicle 2>&1 | tail -3

echo
echo "== 2/3 preflight =="
UNIXNU_UEFI="$UEFI" python3 "$DEV/scripts/check-popsicle.py"

echo
echo "== 3/3 boot image =="
cd "$UNIXNU"
python3 unixnu bootimg \
  -D "$DEV" \
  --uefi "$UEFI" \
  --target RELEASE \
  --fd "$UEFI/Build/popsiclePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd" \
  --dtb "$UEFI/Resources/DTBs/popsicle.dtb" 2>&1 | tail -4

echo
echo "== result =="
ls -l "$UNIXNU/out/popsicle/boot/boot.img"
python3 -c "
import json
d = json.load(open('$UNIXNU/out/popsicle/boot/boot.json'))
for k, v in d.items():
    print(f'  {k}: {v}')
"
