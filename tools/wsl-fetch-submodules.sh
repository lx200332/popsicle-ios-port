#!/bin/bash
# Fetch the 5 required submodules as tarballs (fast, resumable) instead of git clone.
set -u
cd <BUILD>/mu-silicium || exit 1
mkdir -p <BUILD>/dl

fetch() { # $1=url $2=outfile
  local url="$1" out="$2"
  if [ -s "$out" ]; then echo "  cached: $out ($(du -h "$out"|cut -f1))"; return 0; fi
  curl -sSL --http1.1 --retry 8 --retry-all-errors --retry-delay 2 -C - -o "$out" "$url" \
    && echo "  ok: $out ($(du -h "$out"|cut -f1))" || { echo "  FAIL: $url"; return 1; }
}

echo "[1/5] mu_plus release/202608"
fetch "https://codeload.github.com/microsoft/mu_plus/tar.gz/refs/heads/release/202608" <BUILD>/dl/mu_plus.tar.gz
echo "[2/5] mu_oem_sample release/202608"
fetch "https://codeload.github.com/microsoft/mu_oem_sample/tar.gz/refs/heads/release/202608" <BUILD>/dl/mu_oem_sample.tar.gz
echo "[3/5] mu_basecore release/202608"
fetch "https://codeload.github.com/microsoft/mu_basecore/tar.gz/refs/heads/release/202608" <BUILD>/dl/mu_basecore.tar.gz
echo "[4/5] Silicium-ACPI main"
fetch "https://codeload.github.com/Project-Silicium/Silicium-ACPI/tar.gz/refs/heads/main" <BUILD>/dl/silicium-acpi.tar.gz
echo "[5/5] openssl @67b5686"
fetch "https://codeload.github.com/openssl/openssl/tar.gz/67b5686b4419b4cb8caa502711c41815f5279751" <BUILD>/dl/openssl.tar.gz
echo "=== all downloads ==="
ls -la <BUILD>/dl/
