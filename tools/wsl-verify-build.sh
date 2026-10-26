#!/bin/bash
cd /root/build/mu-silicium || exit 1
echo "=== FV output ==="
ls -la Build/popsiclePkg/RELEASE_CLANGPDB/FV/*.fd 2>/dev/null
echo
echo "=== final boot images (build_uefi.py output) ==="
ls -la Build/*.img 2>/dev/null; ls -la *.img 2>/dev/null; find . -maxdepth 2 -name "Mu-popsicle*" -o -maxdepth 2 -name "*.img" 2>/dev/null | head
echo
echo "=== BootShim ==="
find . -name "BootShim*" -type f 2>/dev/null | head -20
echo
echo "=== FD header info ==="
xxd -l 64 Build/popsiclePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd 2>/dev/null
echo
echo "=== size ==="
stat -c '%n %s bytes' Build/popsiclePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd 2>/dev/null
