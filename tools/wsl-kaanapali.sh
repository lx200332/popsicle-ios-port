#!/bin/bash
cd <BUILD>/mu-silicium || exit 1
echo "=== KaanapaliPkg tree (depth2) ==="
find Silicon/Qualcomm/KaanapaliPkg -maxdepth 2 -type d | sort | head -40
echo
echo "=== KaanapaliPkg files ==="
find Silicon/Qualcomm/KaanapaliPkg -maxdepth 1 -type f | sort
echo
echo "=== QcomPkg.dsc.inc (first 60 lines) ==="
head -60 Silicon/Qualcomm/QcomPkg/QcomPkg.dsc.inc
