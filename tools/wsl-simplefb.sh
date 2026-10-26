#!/bin/bash
U=/root/build/mu-silicium
echo "=== SimpleFbDxe exists? ==="
ls -l "$U/Silicon/Silicium/SiliciumPkg/Drivers/SimpleFbDxe/" 2>/dev/null || find "$U/Silicon/Silicium" -ipath "*SimpleFbDxe*" | head
echo
echo "=== framebuffer PCDs in popsicle.dsc ==="
grep -n -i "framebuffer\|FrameBufferBase\|FrameBufferSize\|PcdDisplay\|HorizontalResolution\|VerticalResolution" "$U/Platforms/Xiaomi/popsiclePkg/popsicle.dsc"
echo
echo "=== SimpleFbDxe source: how it gets the FB ==="
F=$(find "$U/Silicon/Silicium" -ipath "*SimpleFbDxe*" -name "*.c" | head -1)
echo "file: $F"
grep -n -i "PcdFrameBuffer\|FixedPcdGet\|frame_buffer\|FrameBufferBase\|gST->ConOut\|AllocatePages" "$F" | head -25
echo
echo "=== SimpleFbDxe.inf PCD refs ==="
grep -n -i "Pcd" "$(dirname "$F")/SimpleFbDxe.inf" | head -20
