#!/bin/bash
# Compare the BootShim sources of the Mu-Silicium checkout and the uefi-sheng checkout.
set -e
echo "=== mu-silicium BootShim dir ==="
ls -l <BUILD>/mu-silicium/BootShim/ 2>/dev/null || echo "no BootShim dir"
echo
echo "=== uefi-sheng BootShim dir ==="
ls -l <BUILD>/uefi-sheng/BootShim/ 2>/dev/null || echo "no BootShim dir"
echo
echo "=== diff of BootShim.S (if both exist) ==="
if [ -f <BUILD>/mu-silicium/BootShim/BootShim.S ] && [ -f <BUILD>/uefi-sheng/BootShim/BootShim.S ]; then
  diff -u <BUILD>/mu-silicium/BootShim/BootShim.S <BUILD>/uefi-sheng/BootShim/BootShim.S | head -80 || true
else
  echo "one side missing"
fi
