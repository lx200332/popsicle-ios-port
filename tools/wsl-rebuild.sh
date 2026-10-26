#!/bin/bash
# Sync the corrected MemoryMapLib.c into the WSL build tree and rebuild the popsicle firmware.
set -e
cd /root/build/mu-silicium
echo "=== descriptor count in tree ==="
grep -c '^  {"' Platforms/Xiaomi/popsiclePkg/Library/MemoryMapLib/MemoryMapLib.c
echo "=== build ==="
python3 build_uefi.py -d popsicle 2>&1 | tail -25
echo "=== artifacts ==="
ls -l Build/popsiclePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd Mu-popsicle.img 2>&1
