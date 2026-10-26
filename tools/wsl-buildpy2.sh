#!/bin/bash
cd /root/build/mu-silicium || exit 1
echo "===== build_uefi.py lines 120-320 ====="
sed -n '120,320p' build_uefi.py
