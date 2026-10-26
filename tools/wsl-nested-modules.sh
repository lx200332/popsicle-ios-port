#!/bin/bash
cd <BUILD>/mu-silicium || exit 1
for d in Mu_Basecore Common/Mu Common/Mu_OEM_Sample Silicium-ACPI Silicon/Silicium; do
  echo "=== $d/.gitmodules ==="
  if [ -f "$d/.gitmodules" ]; then grep -E '^\s*(path|url|branch)' "$d/.gitmodules"; else echo "  (none)"; fi
done
