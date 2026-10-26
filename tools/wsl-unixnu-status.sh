#!/bin/bash
# End-to-end: let the UniXNU driver read the popsicle device repo and pack the boot image itself.
set -e
DEV=/root/build/UniXNU-popsicle
UEFI=/root/build/mu-silicium
UNIXNU=/root/build/unixnu

rm -rf "$DEV"
mkdir -p "$DEV"
cp -r "/mnt/c/dsh-对话/popsicle-port/UniXNU-popsicle/." "$DEV/"
echo "=== device repo ==="
find "$DEV" -type f | sed "s|$DEV/||" | sort
echo
echo "=== unixnu status ==="
cd "$UNIXNU"
python3 unixnu status -D "$DEV" --uefi "$UEFI" 2>&1 | tail -40 || true
