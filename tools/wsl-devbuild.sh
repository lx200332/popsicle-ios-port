#!/bin/bash
cd <BUILD>/mu-silicium || exit 1
echo "=== submodule dirs status ==="
for d in Common/Mu Common/Mu_OEM_Sample Mu_Basecore Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl Silicium-ACPI; do
  n=$(find "$d" -type f 2>/dev/null | wc -l)
  printf '%-70s %s files\n' "$d" "$n"
done
echo
echo "=== DeviceBuild.py ==="
cat Platforms/Xiaomi/popsiclePkg/DeviceBuild.py
