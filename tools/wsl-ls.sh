#!/bin/bash
cd /root/build/mu-silicium || exit 1
echo "=== Silicon/Qualcomm ==="
ls Silicon/Qualcomm/
echo "=== QcomPkg subdirs ==="
ls Silicon/Qualcomm/QcomPkg/ 2>/dev/null
echo "=== QcomPkg/Drivers count ==="
ls Silicon/Qualcomm/QcomPkg/Drivers 2>/dev/null | wc -l
echo "=== QcomPkg/Drivers first 25 ==="
ls Silicon/Qualcomm/QcomPkg/Drivers 2>/dev/null | head -25
