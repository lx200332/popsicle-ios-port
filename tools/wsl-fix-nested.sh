#!/bin/bash
# The nested submodules were extracted relative to workspace root; they belong under Mu_Basecore/.
set -u
cd <BUILD>/mu-silicium || exit 1

while IFS=$'\t' read -r path sha; do
  [ -z "$path" ] && continue
  src="./$path"
  dst="Mu_Basecore/$path"
  if [ -d "$src" ] && [ -n "$(ls -A "$src" 2>/dev/null)" ]; then
    rm -rf "$dst"
    mkdir -p "$(dirname "$dst")"
    mv "$src" "$dst"
    echo "moved $path -> $dst  ($(find "$dst" -type f | wc -l) files)"
  else
    echo "no stray at $path (dst has $(find "$dst" -type f 2>/dev/null | wc -l) files)"
  fi
done < <BUILD>/dl/nested_pins.tsv

echo "=== remove stray top-level dirs if now empty ==="
for d in UnitTestFrameworkPkg MdeModulePkg MdePkg SecurityPkg BaseTools; do
  if [ -d "$d" ]; then
    if [ -z "$(find "$d" -type f 2>/dev/null)" ]; then rm -rf "$d"; echo "  removed empty $d"
    else echo "  KEEP $d ($(find "$d" -type f | wc -l) files)"; fi
  fi
done

echo "=== verify ==="
for p in MdePkg/Library/MipiSysTLib/mipisyst/library/include MdePkg/Library/BaseFdtLib/libfdt; do
  printf '%-58s ' "$p"
  [ -d "Mu_Basecore/$p" ] && echo "OK ($(find "Mu_Basecore/$p" -type f|wc -l) files)" || echo "MISSING"
done
