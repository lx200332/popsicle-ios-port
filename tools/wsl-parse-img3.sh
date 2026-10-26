#!/bin/bash
python3 - <<'PY'
import struct, zlib, gzip, io, hashlib
p = "<BUILD>/mu-silicium/Mu-popsicle.img"
d = open(p,'rb').read()
PS = 4096
ks = struct.unpack_from('<I', d, 8)[0]
rs = struct.unpack_from('<I', d, 12)[0]
kern = d[PS:PS+ks]
print("kernel blob:", len(kern))

dec = zlib.decompressobj(16+zlib.MAX_WBITS)
gzout = dec.decompress(kern)
gzout += dec.flush()
rest = dec.unused_data
print("gzip stream consumed:", len(kern)-len(rest))
print("decompressed FD    :", len(gzout), "bytes")
print("appended data      :", len(rest), "bytes")

# FD checks
idx = gzout.find(b'_FVH')
print()
print("=== UEFI FD ===")
print("_FVH at 0x%x" % idx)
if idx >= 40:
    fvlen  = struct.unpack_from('<Q', gzout, idx-40)[0]
    fssig  = gzout[idx-24:idx-16]
    fvattr = struct.unpack_from('<I', gzout, idx-20)[0]
    hdrsz  = struct.unpack_from('<H', gzout, idx+16)[0]
    rev    = gzout[idx+20]
    print("  FV length = 0x%x (%d)" % (fvlen, fvlen))
    print("  file system GUID = %s" % fssig.hex())
    print("  attributes = 0x%x" % fvattr)
    print("  header length = 0x%x, revision = %d" % (hdrsz, rev))
# FV should start at 0 with 16 zero bytes then GUID then len
print("  first 16 bytes:", gzout[:16].hex())
print("  FS GUID at 16 :", gzout[16:32].hex())
print("  total decompressed:", len(gzout))

print()
print("=== appended DTB ===")
if rest[:4] == b'\xd0\x0d\xfe\xed':
    tot, off_struct, off_str, off_rsv, ver, lastcomp, bootcpu, size_str, size_struct = struct.unpack_from('>9I', rest, 4)
    print("  FDT magic OK")
    print("  totalsize   = 0x%x (%d)" % (tot, tot))
    print("  version     = %d, last_comp = %d, boot_cpuid = %d" % (ver, lastcomp, bootcpu))
    print("  off_struct  = 0x%x, size_struct = 0x%x" % (off_struct, size_struct))
    print("  off_strings = 0x%x, size_strings = 0x%x" % (off_str, size_str))
    # find model
    s = rest[:tot]
    for key in (b'popsicle', b'Xiaomi 17 Pro Max', b'qcom,canoe', b'kaanapali'):
        i = s.find(key)
        print("  %-20s at 0x%x" % (key.decode(), i))
    print("  md5:", hashlib.md5(s).hexdigest())
    print("  dtb length matches expected 1723024:", tot == 1723024)
else:
    print("  magic:", rest[:8].hex())

print()
print("=== ramdisk ===")
rd = d[PS+((ks+PS-1)//PS)*PS:][:rs]
print("  bytes:", rd.hex(), "->", rd)

print()
print("=== trailing ===")
tail_off = PS + ((ks+PS-1)//PS)*PS + ((rs+PS-1)//PS)*PS
print("  offset 0x%x, len %d" % (tail_off, len(d)-tail_off))
print("  all zero:", all(b==0 for b in d[tail_off:]))
PY
