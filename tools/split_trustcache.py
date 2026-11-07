#!/usr/bin/env python3
"""split_trustcache.py -- split an embedded trust cache bundle into its modules.

    split_trustcache.py BOOT.img OUTDIR

A pandora boot image carries the trust cache as one payload segment holding a
*bundle*: u32 num_caches, u32 offsets[], then each cache. addtrustcache.py only
takes single caches, so the bundle has to be opened first.

Module 0 is the static cache from the IPSW; module 1 is the small one that carries
the patched third-party/NativeAdreno cdhashes. Entries inside a module are not
guaranteed sorted in the wild (ct5's module 1 has one out-of-order pair), and
addtrustcache.py asserts on that, so this writes module 1 back sorted.
"""
import os
import struct
import sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import extract_adt  # reuse the boot-image reader

PAGE = 0x4000

def payload(raw):
    import gzip
    if raw[:2] == b'\x1f\x8b':
        raw = gzip.decompress(raw)
    if raw[:8] != b'ANDROID!':
        raise SystemExit('not an Android boot image')
    ksize = struct.unpack('<I', raw[8:12])[0]
    return raw[0x1000:0x1000 + ksize][0xb000:]


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__)
    src, outdir = argv
    os.makedirs(outdir, exist_ok=True)
    p = payload(open(src, 'rb').read())
    if p[:8] != b'SWEETXNU':
        raise SystemExit('no SWEETXNU payload')
    nsegs = struct.unpack('<I', p[0xc:0x10])[0]
    # The trust cache is the last segment: {vmaddr, vmsize, file_off, file_size}.
    seg = struct.unpack('<4Q', p[0x2a8 + 32 * (nsegs - 1):0x2a8 + 32 * nsegs])
    off, size = seg[2], seg[3]
    bundle = p[off:off + size]
    n = struct.unpack('<I', bundle[:4])[0]
    offsets = list(struct.unpack('<%dI' % n, bundle[4:4 + 4 * n]))
    print(f'bundle: {n} modules, {size} bytes', file=sys.stderr)
    for i, o in enumerate(offsets):
        end = offsets[i + 1] if i + 1 < n else size
        body = bundle[o:end]
        body = body[:24 * struct.unpack('<I', body[20:24])[0] + 24]
        path = f'{outdir}/tc-mod{i}.bin'
        open(path, 'wb').write(body)
        entries = [body[24 + 24 * k:24 + 24 * (k + 1)] for k in range((len(body) - 24) // 24)]
        hashes = [e[0:20] for e in entries]
        print(f'  mod{i}: {len(entries)} entries, sorted={hashes == sorted(hashes)}', file=sys.stderr)
        if hashes != sorted(hashes):
            order = sorted(range(len(hashes)), key=lambda k: hashes[k])
            fixed = body[:24] + b''.join(entries[k] for k in order)
            open(path, 'wb').write(fixed)
            print(f'  mod{i}: re-sorted and rewritten', file=sys.stderr)


if __name__ == '__main__':
    main(sys.argv[1:])
