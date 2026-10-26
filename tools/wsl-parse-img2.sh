#!/bin/bash
cd <BUILD>/mu-silicium || exit 1
echo "=== Resources/ramdisk ==="
ls -la Resources/ramdisk 2>/dev/null; stat -c '%s' Resources/ramdisk 2>/dev/null
echo
echo "=== raw header bytes 0..96 ==="
od -A d -t x1z -v -N 96 Mu-popsicle.img
echo
python3 - <<'PY'
import struct, gzip
p = "<BUILD>/mu-silicium/Mu-popsicle.img"
d = open(p,'rb').read()
PS = 4096
magic = d[0:8]
kernel_size  = struct.unpack_from('<I', d, 8)[0]
ramdisk_size = struct.unpack_from('<I', d, 12)[0]
os_version   = struct.unpack_from('<I', d, 16)[0]
header_size  = struct.unpack_from('<I', d, 20)[0]
reserved     = struct.unpack_from('<4I', d, 24)
header_ver   = struct.unpack_from('<I', d, 40)[0]
cmdline      = d[44:44+1536].split(b'\x00')[0].decode(errors='replace')
print("magic        =", magic)
print("kernel_size  =", kernel_size, hex(kernel_size))
print("ramdisk_size =", ramdisk_size)
print("os_version   =", os_version)
print("header_size  =", header_size)
print("reserved     =", reserved)
print("header_ver   =", header_ver)
print("cmdline      =", repr(cmdline))
print("total        =", len(d))

def npages(n): return (n + PS - 1)//PS
off = PS
kern = d[off:off+kernel_size]; off += npages(kernel_size)*PS
rd   = d[off:off+ramdisk_size]; off += npages(ramdisk_size)*PS
print()
print("kernel @0x%x len %d  first4=%s" % (PS, len(kern), kern[:4].hex()))
print("ramdisk @0x%x len %d" % (PS+npages(kernel_size)*PS, len(rd)))
print("trailing bytes after ramdisk:", len(d) - off, "at 0x%x" % off)

if kern[:2] == b'\x1f\x8b':
    gz = gzip.decompress(kern)
    print()
    print("kernel gzip OK ->", len(gz), "bytes")
    idx = gz.find(b'_FVH')
    print("  _FVH at 0x%x" % idx)
    if idx >= 40:
        fvlen = struct.unpack_from('<Q', gz, idx-40)[0]
        print("  FV length = 0x%x (%d)" % (fvlen, fvlen))
    # dtb appended after gzip stream inside kernel? check FDT magic in gz tail
    fi = gz.find(b'\xd0\x0d\xfe\xed')
    print("  FDT magic in decompressed at 0x%x" % fi)
    # also check whether the raw kernel has FDT after the gzip member
    import zlib
    dec = zlib.decompressobj(16+zlib.MAX_WBITS)
    out = dec.decompress(kern)
    rest = dec.unused_data
    print("  gzip member consumed %d, unused_data %d bytes" % (len(kern)-len(rest), len(rest)))
    if rest[:4] == b'\xd0\x0d\xfe\xed':
        tot = struct.unpack_from('>I', rest, 4)[0]
        print("  appended DTB: totalsize=0x%x (%d)" % (tot, tot))
        i = rest.find(b'popsicle')
        print("  'popsicle' at 0x%x in dtb" % i)
    else:
        print("  unused_data magic:", rest[:8].hex())
PY
