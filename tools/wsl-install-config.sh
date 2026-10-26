#!/bin/bash
# Install popsicle config + DTB into the WSL build tree.
set -e
cd <BUILD>/mu-silicium || exit 1

# 1) Device config
cat > Resources/Configs/popsicle.toml <<'EOF'
#
# Boot Shim Configs
#
[boot_shim]
requires_kernel_header = true

#
# UEFI FD Configs
#
[uefi_fd]
base = 0xC6800000
size = 0x400000

#
# Android Boot Image Kernel Configs
#
[boot_image_kernel]
kernel_compression = "gzip"
append_dtb         = true

#
# Android Boot Image Configs
#
[boot_image]
header_version = 4
EOF
echo "wrote Resources/Configs/popsicle.toml"

# 2) Device DTB (merged popsicle DTB)
SRC_DTB="/mnt<WORKSPACE>/popsicle-port/popsicle-port/dts/popsicle_merged.dtb"
ALT_DTB="/mnt<WORKSPACE>/mu-silicium/Platforms/Xiaomi/popsiclePkg/FdtBlob/kaanapali-xiaomi-popsicle.dtb"
if [ -f "$ALT_DTB" ]; then
  cp -f "$ALT_DTB" Resources/DTBs/popsicle.dtb
  echo "copied DTB from popsiclePkg/FdtBlob"
elif [ -f "$SRC_DTB" ]; then
  cp -f "$SRC_DTB" Resources/DTBs/popsicle.dtb
  echo "copied DTB from popsicle-port/dts"
else
  echo "ERROR: no source DTB found"; exit 1
fi
ls -la Resources/DTBs/popsicle.dtb
echo "=== md5 compare ==="
md5sum Resources/DTBs/popsicle.dtb Platforms/Xiaomi/popsiclePkg/FdtBlob/kaanapali-xiaomi-popsicle.dtb 2>/dev/null
