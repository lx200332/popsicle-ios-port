#!/bin/bash
cd /root/build/mu-silicium || exit 1
echo "=== BootShim artifacts ==="
ls -la BootShim/
echo
echo "=== BootShim.bin first 112 bytes ==="
od -A d -t x1z -v -N 112 BootShim/BootShim.bin 2>/dev/null
echo
python3 - <<'PY'
import struct, zlib
bs = open("/root/build/mu-silicium/BootShim/BootShim.bin","rb").read()
print("BootShim.bin size =", len(bs))
d = open("/root/build/mu-silicium/Mu-popsicle.img","rb").read()
ks = struct.unpack_from('<I', d, 8)[0]
kern = d[4096:4096+ks]
dec = zlib.decompressobj(16+zlib.MAX_WBITS)
out = dec.decompress(kern); out += dec.flush()
print("decompressed total =", len(out))
prefix = out[:len(out)-4194304]
print("prefix length =", len(prefix))
print("prefix hex    =", prefix.hex())
print("matches BootShim.bin:", prefix == bs)
fd = out[len(prefix):]
print("FD length =", len(fd), hex(len(fd)))
# FD header sanity
print("FD first 16 (zero vector):", fd[:16].hex())
print("FD FS GUID:", fd[16:32].hex())
fvlen = struct.unpack_from('<Q', fd, 32)[0]
print("FD FV length:", hex(fvlen))
print("FD _FVH at 40:", fd[40:44])
PY
