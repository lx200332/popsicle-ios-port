#!/bin/bash
# Verify the EDK2 build toolchain inside WSL.
for t in iasl nasm dtc aarch64-linux-gnu-gcc python3 make git gcc clang; do
  p=$(command -v "$t" 2>/dev/null)
  printf '%-26s %s\n' "$t" "${p:-MISSING}"
done
echo "--- versions ---"
python3 --version 2>&1
gcc --version 2>&1 | head -1
make --version 2>&1 | head -1
free -g | head -2
df -h / | tail -1
