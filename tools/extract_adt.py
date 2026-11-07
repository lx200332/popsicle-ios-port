#!/usr/bin/env python3
"""extract_adt.py -- pull the ADT out of a pandora boot image.

    extract_adt.py BOOT.img OUT.adt

The device-pinned /chosen state (UFS root/log/fw block extents, Wi-Fi MAC) is not
something that can be computed — it was captured from the phone by an earlier boot
and lives on inside that boot image's payload. Carry it forward with
`make_image.sh --ref-adt OUT.adt` instead of inventing values.

    extract_adt.py boots/loader-ct5-boot.img.gz ref.adt   # gzip is fine too
"""
import gzip
import struct
import sys

PAYLOAD_OFF = 0xb000  # inside the arm64 Image, after the loader


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__)
    src, dst = argv
    raw = open(src, 'rb').read()
    if raw[:2] == b'\x1f\x8b':
        raw = gzip.decompress(raw)
    if raw[:8] != b'ANDROID!':
        raise SystemExit(f'{src}: not an Android boot image')
    ksize = struct.unpack('<I', raw[8:12])[0]
    img = raw[0x1000:0x1000 + ksize]
    p = img[PAYLOAD_OFF:]
    if p[:8] != b'SWEETXNU':
        raise SystemExit(f'{src}: no SWEETXNU payload at Image+0x{PAYLOAD_OFF:x}')
    adt_off, adt_size = struct.unpack('<QQ', p[0x20:0x30])
    blob = p[adt_off:adt_off + adt_size]
    if len(blob) != adt_size:
        raise SystemExit(f'{src}: ADT truncated')
    open(dst, 'wb').write(blob)
    print(f'{dst}: {len(blob)} bytes')


if __name__ == '__main__':
    main(sys.argv[1:])
