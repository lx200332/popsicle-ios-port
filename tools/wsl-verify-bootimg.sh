#!/bin/bash
# Verify the UniXNU-packed popsicle boot image: header fields and gzip payload layout.
set -e
python3 - <<'PY'
import struct, zlib, hashlib
p = '/root/build/unixnu/out/popsicle/boot/boot.img'
d = open(p, 'rb').read()
print('file bytes   ', len(d))
magic, ksize, kaddr, rsize, raddr, ssize, saddr, taddr, page, ver = struct.unpack_from('<8s9I', d, 0)
print('magic        ', magic, '| header_version', ver, '| page_size', page)
print('kernel_size  ', ksize, 'kernel_addr', hex(kaddr))
print('ramdisk_size ', rsize, 'ramdisk_addr', hex(raddr))
print('second_size  ', ssize, 'tags_addr', hex(taddr))
k = d[page:page+ksize]
do = zlib.decompressobj(31)
payload = do.decompress(k)
unused = do.unused_data
print()
print('gzip consumed', len(k) - len(unused), 'bytes ->', len(payload), 'bytes payload')
print('shim         192 bytes; fd =', len(payload) - 192, 'bytes (expect 4194304)')
print('  text_offset @8  =', hex(struct.unpack_from('<Q', payload, 8)[0]))
print('  image_size  @16 =', struct.unpack_from('<Q', payload, 16)[0], '(expect 192+4194304 =', 192+4194304, ')')
print('  ARM64 magic @56 =', payload[56:60])
print('  fd sha256       =', hashlib.sha256(payload[192:]).hexdigest())
print()
print('appended dtb  ', len(unused), 'bytes, magic', unused[:4].hex(), '(d00dfeed)')
print('  dtb totalsize ', hex(struct.unpack_from('>I', unused, 4)[0]))
print('  dtb sha256    ', hashlib.sha256(unused).hexdigest())
print()
tail = d[page+ksize:]
print('bytes after kernel field:', len(tail), '(expect page-aligned padding to', ((page+ksize+page-1)//page)*page, ')')
PY
