#!/bin/bash
cd /root/build/mu-silicium/Mu_Basecore/MdePkg/Library/MipiSysTLib || exit 1
echo "=== MipiSysTLib dir ==="; ls -la
echo "=== mipisyst dir ==="; ls -la mipisyst 2>/dev/null | head -20
echo "=== looking for library/include ==="
find mipisyst -maxdepth 3 -type d 2>/dev/null | head -20
echo "=== MdePkg.dec line 33 context ==="
sed -n '25,45p' /root/build/mu-silicium/Mu_Basecore/MdePkg/MdePkg.dec
