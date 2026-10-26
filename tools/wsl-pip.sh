#!/bin/bash
set -e
cd /root/build/mu-silicium || exit 1
echo "=== python ==="
python3 --version
which python3
echo "=== install pip requirements ==="
python3 -m pip install -r pip-requirements.txt --break-system-packages 2>&1 | tail -15
echo "=== verify key tools ==="
for t in clang lld ld.lld nasm make gcc aarch64-linux-gnu-gcc mono; do
  printf '%-24s ' "$t"
  command -v "$t" >/dev/null 2>&1 && echo "OK $(command -v $t)" || echo "MISSING"
done
