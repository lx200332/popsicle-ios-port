#!/bin/bash
cd /root/build/mu-silicium || exit 1
export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
python3 build_uefi.py -d popsicle 2>&1 | tail -80
echo "BUILD_EXIT=${PIPESTATUS[0]}"
