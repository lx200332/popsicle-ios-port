#!/bin/bash
cd <BUILD>/mu-silicium || exit 1
IMG=Mu-popsicle.img
echo "=== file header (Android boot v4) ==="
xxd -l 16 "$IMG"
echo "magic should be ANDROID!"
echo
echo "=== parse header v4 (python) ==="
python3 - "$IMG" <<'PY'
import sys, struct, gzip, zlib, io, hashlib
p = sys.argv[1]
d = open(p,'rb').read()
print("total size", len(d))
magic = d[0:8]
print("magic", magic)
# Android boot header v4
# struct boot_img_hdr_v4:
# 0: magic[8]
# 8: kernel_size u32
# 12: kernel_addr u32
# 16: ramdisk_size u32
# 20: ramdisk_addr u32
# 24: second_size u32
# 28: second_addr u32
# 32: tags_addr u32
# 36: page_size u32
# 40: header_version u32
# 44: os_version u32
# 48: name[16]
# 64: cmdline[512]
# 576: id[8]
# 584: extra_cmdline[1024]
# 1608: recovery_dtbo_size u32
# 1612: recovery_dtbo_offset u64
# 1620: header_size u32
# 1624: dtb_size u32
# 1628: dtb_addr u64
kernel_size, kernel_addr = struct.unpack_from('<II', d, 8)
ramdisk_size, ramdisk_addr = struct.unpack_from('<II', d, 16)
page_size = struct.unpack_from('<I', d, 36)[0]
header_version = struct.unpack_from('<I', d, 40)[0]
name = d[48:64].rstrip(b'\x00').decode(errors='replace')
cmdline = d[64:576].rstrip(b'\x00').decode(errors='replace')
dtb_size = struct.unpack_from('<I', d, 1624)[0]
dtb_addr = struct.unpack_from('<Q', d, 1628)[0]
header_size = struct.unpack_from('<I', d, 1620)[0]
print(f"kernel_size   = {kernel_size} (0x{kernel_size:x})")
print(f"kernel_addr   = 0x{kernel_addr:x}")
print(f"ramdisk_size  = {ramdisk_size} (0x{ramdisk_size:x})")
print(f"ramdisk_addr  = 0x{ramdisk_addr:x}")
print(f"page_size     = {page_size}")
print(f"header_version= {header_version}")
print(f"header_size   = {header_size}")
print(f"dtb_size      = {dtb_size} (0x{dtb_size:x})")
print(f"dtb_addr      = 0x{dtb_addr:x}")
print(f"name          = {name!r}")
print(f"cmdline       = {cmdline!r}")

def pages(n, ps=page_size): return (n + ps - 1)//ps
off = page_size
kern = d[off:off+kernel_size]; off += pages(kernel_size)*page_size
rd   = d[off:off+ramdisk_size]; off += pages(ramdisk_size)*page_size
dtb  = d[off:off+dtb_size]
print()
print("kernel magic bytes:", kern[:4].hex())
# kernel should be gzip
if kern[:2] == b'\x1f\x8b':
    try:
        gz = gzip.decompress(kern)
        print(f"kernel gzip OK -> decompressed {len(gz)} bytes")
        print("  inner magic:", gz[:8].hex())
        # UEFI FD: _FVH at offset 0x28
        idx = gz.find(b'_FVH')
        print(f"  _FVH at offset {idx} (0x{idx:x})")
        if idx >= 0:
            fvlen = struct.unpack_from('<Q', gz, idx-40)[0] if idx>=40 else 0
            print(f"  FV length field = 0x{fvlen:x}")
    except Exception as e:
        print("gzip decompress failed:", e)
if dtb[:4] == b'\xd0\x0d\xfe\xed':
    totalsize = struct.unpack_from('>I', dtb, 4)[0]
    print(f"dtb FDT magic OK, totalsize=0x{totalsize:x} ({totalsize})")
    # model string
    i = dtb.find(b'popsicle')
    print("  'popsicle' at", i)
else:
    print("dtb magic:", dtb[:4].hex())
print()
print("ramdisk size:", len(rd))
PY
echo
echo "=== files in tree root ==="
ls -la | head -30
