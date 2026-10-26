#!/bin/bash
cd /root/build/mu-silicium || exit 1
echo "===== platforms using KaanapaliPkg ====="
grep -rl "KaanapaliPkg/KaanapaliPkg.dsc.inc" Platforms/ 2>/dev/null
echo
echo "===== platforms using LahainaPkg (for contrast) ====="
grep -rl "LahainaPkg/LahainaPkg.dsc.inc" Platforms/ 2>/dev/null | head -5
echo
echo "===== all Xiaomi platforms ====="
ls Platforms/Xiaomi/
echo
echo "===== Status.md ====="
head -80 Status.md
