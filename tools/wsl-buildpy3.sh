#!/bin/bash
cd <BUILD>/mu-silicium || exit 1
echo "===== build_uefi.py 320-end ====="
sed -n '320,500p' build_uefi.py
echo
echo "===== infiniti.toml ====="
cat Resources/Configs/infiniti.toml
echo
echo "===== popsicle.toml ====="
cat Resources/Configs/popsicle.toml 2>/dev/null || echo "MISSING popsicle.toml"
echo
echo "===== Resources/DTBs ====="
ls Resources/DTBs | head -20
echo "count: $(ls Resources/DTBs | wc -l)"
