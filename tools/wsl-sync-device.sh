#!/bin/bash
# Mirror the Windows-side UniXNU-popsicle device repo into the WSL build tree.
#
# Run from WINDOWS bash (ops_bash), not inside WSL. Two problems are solved here:
#
#   1. `/mnt/c` only exists inside WSL, so this cannot cp directly.
#   2. The workspace path contains non-ASCII characters (C:\dsh-对话), and passing those through
#      wsl.exe mangles them into $'\345\257\271\350\257\235' escapes. Staging through an ASCII-only
#      directory keeps every path WSL sees plain ASCII.
#
# STAGE must be on the Windows filesystem (WSL reaches it as /mnt/c/...). Keep it outside the workspace
# so the device repo is never confused with a build artifact.
set -e

SRC_WIN='C:\dsh-对话\popsicle-port\UniXNU-popsicle'
STAGE_WIN='C:\dsh-tools\cache\popsicle-sync'
STAGE_WSL='/mnt/c/dsh-tools/cache/popsicle-sync'
DST_WSL='/root/build/UniXNU-popsicle'

rm -rf "$STAGE_WIN"
mkdir -p "$STAGE_WIN"
cp -a "$SRC_WIN"/. "$STAGE_WIN"/
echo "staged $(find "$STAGE_WIN" -type f | wc -l) files into $STAGE_WIN"

wsl.exe -d Ubuntu-24.04 -- bash -lc "set -e
mkdir -p '$DST_WSL'
rm -rf '$DST_WSL'/*
cp -a '$STAGE_WSL'/. '$DST_WSL'/
echo '=== synced files ==='
find '$DST_WSL' -type f -printf '%s\t%P\n' | sort -k2
echo
echo '=== crypto bootstrap mode in the synced defconfig ==='
grep 'CRYPTO_BOOTSTRAP' '$DST_WSL/configs/popsicle_defconfig'
echo
echo '=== prelink block present? ==='
grep -c '^\[kernel.prelink\]' '$DST_WSL/board.toml'
" 2>&1 | tr -d '\000' | grep -v 'localhost'
