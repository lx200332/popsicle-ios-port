#!/bin/bash
# Full recursive clone of Mu-Silicium inside WSL (Linux native FS = fast build).
set -x
export http_proxy=http://172.27.32.1:7897
export https_proxy=http://172.27.32.1:7897

cd /root || exit 1
rm -rf Mu-Silicium

git config --global http.sslVerify true
git config --global http.postBuffer 524288000
git config --global core.compression 0

git clone --recursive --depth 1 --shallow-submodules \
  https://github.com/Project-Silicium/Mu-Silicium.git Mu-Silicium 2>&1 | tail -20

echo "EXIT=$?"
cd Mu-Silicium || exit 1
echo "=== files ==="
find . -path ./.git -prune -o -type f -print | wc -l
echo "=== size ==="
du -sh .
echo "=== key dirs ==="
for d in Binaries Common/Mu Common/Mu_OEM_Sample Mu_Basecore Silicon/Silicium Silicon/Qualcomm Silicium-ACPI BootShim; do
  printf '%-26s ' "$d"
  if [ -d "$d" ]; then echo "ok ($(ls -A $d 2>/dev/null | wc -l))"; else echo MISSING; fi
done
echo "=== DONE ==="
