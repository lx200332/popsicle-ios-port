#!/bin/bash
cd <BUILD>/mu-silicium || exit 1
export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
echo "=== stuart_update via DeviceBuild.py --update ==="
python3 Platforms/Xiaomi/popsiclePkg/DeviceBuild.py --update 2>&1 | tail -60
echo "UPDATE_EXIT=${PIPESTATUS[0]}"
