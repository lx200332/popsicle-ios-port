#!/bin/bash
# Install the generated popsicle panel XMLs into Binaries/popsicle/RawFiles and rebuild.
set -e
UEFI=<BUILD>/mu-silicium
RAW="$UEFI/Binaries/popsicle/RawFiles"
SRC="/mnt<WORKSPACE>/popsicle-port/panels"

for f in Panel_p2_42_0d_0b_mp_prim_dsc_cmd.xml Panel_p2_42_06_0a_sec_dsc_cmd.xml; do
  cp "$SRC/$f" "$RAW/$f"
  echo "installed $f ($(stat -c%s "$RAW/$f") bytes)"
done

echo
echo "=== panel xmls now present for p2_42 ==="
ls -1 "$RAW" | grep -i 'p2_42' || echo "none"

echo
echo "=== rebuild ==="
cd "$UEFI"
python3 build_uefi.py -d popsicle 2>&1 | tail -6
ls -l Build/popsiclePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd Mu-popsicle.img
