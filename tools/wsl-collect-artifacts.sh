#!/bin/bash
# Copy the finished artifacts out of WSL and report adb state.
set -e
ART="/mnt<WORKSPACE>/popsicle-port/artifacts"
mkdir -p "$ART"
cp <BUILD>/unixnu/out/popsicle/boot/boot.img "$ART/boot-popsicle-unixnu.img"
cp <BUILD>/unixnu/out/popsicle/boot/boot.json "$ART/boot-popsicle-unixnu.json"
cp <BUILD>/mu-silicium/Mu-popsicle.img "$ART/Mu-popsicle-musilicium.img"
cp <BUILD>/mu-silicium/Build/popsiclePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd "$ART/SILICIUM_UEFI.fd"
cp <BUILD>/mu-silicium/BootShim/BootShim.bin "$ART/BootShim.bin"
ls -l "$ART"
echo
echo "=== sha256 ==="
sha256sum "$ART"/*.img "$ART"/*.fd "$ART"/*.bin 2>/dev/null
