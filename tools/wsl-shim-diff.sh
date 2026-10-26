#!/bin/bash
# Compare the BootShim sources of the Mu-Silicium checkout and the uefi-sheng checkout.
set -e
echo "=== mu-silicium BootShim dir ==="
ls -l /root/build/mu-silicium/BootShim/ 2>/dev/null || echo "no BootShim dir"
echo
echo "=== uefi-sheng BootShim dir ==="
ls -l /root/build/uefi-sheng/BootShim/ 2>/dev/null || echo "no BootShim dir"
echo
echo "=== diff of BootShim.S (if both exist) ==="
if [ -f /root/build/mu-silicium/BootShim/BootShim.S ] && [ -f /root/build/uefi-sheng/BootShim/BootShim.S ]; then
  diff -u /root/build/mu-silicium/BootShim/BootShim.S /root/build/uefi-sheng/BootShim/BootShim.S | head -80 || true
else
  echo "one side missing"
fi
