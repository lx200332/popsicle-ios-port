#!/bin/bash
set -e
cd /root/build
export GIT_HTTP_LOW_SPEED_LIMIT=0
export GIT_HTTP_LOW_SPEED_TIME=999
rm -rf devbin
git -c http.version=HTTP/1.1 \
    -c http.postBuffer=524288000 \
    clone --filter=blob:none --sparse --depth 1 \
    https://github.com/Project-Silicium/Device-Binaries.git devbin 2>&1 | tail -8
cd devbin
git sparse-checkout set infiniti 2>&1 | tail -5
echo "=== SPARSE OK ==="
du -sh .
echo "=== infiniti top ==="
ls infiniti
echo "=== tree depth 3 ==="
find infiniti -maxdepth 3 -type d | head -30
echo "=== file count ==="
find infiniti -type f | wc -l
