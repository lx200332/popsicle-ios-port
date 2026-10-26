#!/bin/bash
U=/root/build/unixnu
echo "=== target handling in cli.py ==="
grep -n "target" "$U/scripts/unixnu/cli.py" | head -30
echo
echo "=== target handling in firmware.py ==="
grep -n "target" "$U/scripts/unixnu/firmware.py" | head -30
echo
echo "=== target handling in config.py ==="
grep -n "target" "$U/scripts/unixnu/config.py" | head -20
echo
echo "=== how sheng handles it: firmware.fd template ==="
grep -n "fd *=" /root/build/UniXNU-popsicle/board.toml
echo
echo "=== what build dirs exist ==="
ls -d /root/build/mu-silicium/Build/popsiclePkg/*/ 2>/dev/null
