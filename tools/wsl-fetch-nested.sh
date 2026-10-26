#!/bin/bash
set -u
cd <BUILD>/mu-silicium || exit 1
mkdir -p <BUILD>/dl/nested

declare -A URLS=(
  ["UnitTestFrameworkPkg/Library/CmockaLib/cmocka"]="tianocore/edk2-cmocka"
  ["MdeModulePkg/Universal/RegularExpressionDxe/oniguruma"]="kkos/oniguruma"
  ["MdeModulePkg/Library/BrotliCustomDecompressLib/brotli"]="google/brotli"
  ["BaseTools/Source/C/BrotliCompress/brotli"]="google/brotli"
  ["UnitTestFrameworkPkg/Library/GoogleTestLib/googletest"]="google/googletest"
  ["UnitTestFrameworkPkg/Library/SubhookLib/subhook"]="tianocore/edk2-subhook"
  ["MdePkg/Library/BaseFdtLib/libfdt"]="devicetree-org/pylibfdt"
  ["MdePkg/Library/MipiSysTLib/mipisyst"]="MIPI-Alliance/public-mipi-sys-t"
  ["SecurityPkg/DeviceSecurity/SpdmLib/libspdm"]="DMTF/libspdm"
)

while IFS=$'\t' read -r path sha; do
  [ -z "$path" ] && continue
  repo="${URLS[$path]}"
  slug=$(echo "$path" | tr '/' '_')
  tb="<BUILD>/dl/nested/$slug.tar.gz"
  echo "=== $path  ($repo @ ${sha:0:12}) ==="
  if [ ! -s "$tb" ]; then
    curl -sSL --http1.1 --retry 6 --retry-all-errors -C - -o "$tb" \
      "https://codeload.github.com/$repo/tar.gz/$sha" || { echo "  DOWNLOAD FAIL"; continue; }
  fi
  rm -rf "$path"; mkdir -p "$path"
  if tar xzf "$tb" -C "$path" --strip-components=1 2>/dev/null; then
    echo "  extracted $(find "$path" -type f | wc -l) files"
  else
    echo "  EXTRACT FAIL ($(du -h "$tb"|cut -f1))"
  fi
done < <BUILD>/dl/nested_pins.tsv

echo
echo "=== verify the one that blocked us ==="
ls Mu_Basecore/MdePkg/Library/MipiSysTLib/mipisyst/ 2>/dev/null | head
ls Mu_Basecore/MdePkg/Library/BaseFdtLib/libfdt/ 2>/dev/null | head -5
