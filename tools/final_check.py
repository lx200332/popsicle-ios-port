#!/usr/bin/env python3
"""Final pre-flight check for a popsicle loader boot image.

    python3 final_check.py IMAGE

Verifies the boot header, the SWEETXNU payload header, the segment table, the
cmdline budget, the prelinked kext strings and the three 1200x2608 adaptations.
Exit code 0 only when every check passes.
"""
import struct
import sys

PAGE = 0x4000            # arm64 16K granule, used inside the kernel
BOOT_PAGE = 4096         # Android boot image page (mkbootimg_v4.py)
PAYLOAD_IN_IMAGE = 0xB000
BOOT_LINE_LENGTH = 608
SEG_TABLE = 0x2A8


def main():
    path = sys.argv[1]
    d = open(path, 'rb').read()
    ok = True

    def check(cond, label, detail=''):
        nonlocal ok
        ok = ok and bool(cond)
        print(f'{"PASS" if cond else "FAIL"} {label}{"  " + detail if detail else ""}')

    check(d[:8] == b'ANDROID!', 'boot image magic')
    kernel_size, = struct.unpack_from('<I', d, 8)
    check(struct.unpack_from('<I', d, 0x24)[0] in (0, BOOT_PAGE), 'header page_size',
          hex(struct.unpack_from('<I', d, 0x24)[0]))
    total = 0x1000 + ((kernel_size + BOOT_PAGE - 1) & ~(BOOT_PAGE - 1))
    check(len(d) == total, 'file length matches header', f'{len(d)} == {total}')

    image = d[0x1000:0x1000 + kernel_size]
    check(image[56:60] == b'ARM\x64', 'arm64 Image magic')
    check(struct.unpack_from('<Q', image, 16)[0] == len(image), 'image_size == kernel length')

    p = image[PAYLOAD_IN_IMAGE:]
    check(p[:8] == b'SWEETXNU', 'payload magic', hex(PAYLOAD_IN_IMAGE))
    version, nsegs = struct.unpack_from('<II', p, 8)
    check(version == 1, 'payload version', str(version))
    virt_base, entry, adt_off, adt_size, kernel_end = struct.unpack_from('<5Q', p, 0x10)
    phys_base, mem_size = struct.unpack_from('<2Q', p, 0x38)
    cmdline = p[0x48:0x48 + BOOT_LINE_LENGTH].split(b'\0')[0].decode()

    print(f'     nsegs={nsegs} virt_base={virt_base:#x} entry={entry:#x}')
    print(f'     adt_off={adt_off:#x} adt_size={adt_size} kernel_end={kernel_end:#x}')
    print(f'     phys_base={phys_base:#x} mem_size={mem_size:#x}')
    print(f'     cmdline {len(cmdline)}/{BOOT_LINE_LENGTH} bytes')

    check(cmdline.encode() == cmdline.encode()[:BOOT_LINE_LENGTH - 1], 'cmdline fits')
    check(virt_base <= min(struct.unpack_from('<Q', p, SEG_TABLE + 32 * i)[0]
                           for i in range(nsegs)), 'virt_base below every segment')
    check((virt_base & (0x2000000 - 1)) == 0, 'virt_base 32 MB aligned')
    check(kernel_end == (kernel_end + PAGE - 1) & ~(PAGE - 1), 'kernel_end page aligned')
    check(adt_size > 0 and adt_off + adt_size <= len(p), 'ADT inside payload')

    segs = [struct.unpack_from('<4Q', p, SEG_TABLE + 32 * i) for i in range(nsegs)]
    hdr_size = SEG_TABLE + 32 * nsegs          # seg[0] starts here; not 16-byte aligned by design
    bad = [i for i, (va, size, off, n) in enumerate(segs)
           if off < hdr_size or off + n > len(p)]
    check(not bad, 'segment file ranges inside payload', str(bad))
    tc = segs[-1]
    check(tc[2] + tc[3] <= len(p), 'trust cache segment inside payload')
    ncaches, = struct.unpack_from('<I', p, tc[2])
    check(ncaches in (1, 2), 'trust cache module count', str(ncaches))

    for s, label in ((b'AppleMobileFileIntegrityUserClient', 'AMFI user client'),
                     (b'com.apple.driver.AppleMobileFileIntegrity', 'AMFI kext'),
                     (b'com.apple.security.AppleImage4', 'AppleImage4 kext'),
                     (b'PANDORA GPU SUBMIT', 'GPU driver'),
                     (b'FUXI WLAN: enabled', 'fuxi wlan'),
                     (b'PANDORA TOUCH', 'touch'),
                     (b'PANDORA ADSP', 'ADSP'),
                     (b'PANDORA THERM', 'thermal'),
                     (b'XppleARMBacklight', 'renamed backlight class')):
        check(d.count(s) >= 1, f'string present: {label}', str(d.count(s)))

    # AMFI CT-policy bypass: without it the GPU plugin is rejected and everything falls back to
    # CPU compositing (3~4 fps). 2026-11-04: the whole pipeline was rebuilt without this and looked
    # "fine" to every other check.
    def word_at(va):
        for (vaddr, vsize, off, fsz) in segs:
            if vaddr <= va < vaddr + vsize:
                d_ = va - vaddr
                if d_ + 4 > fsz:
                    return None
                return struct.unpack_from('<I', p, off + d_)[0]
        return None
    for va in (0xfffffff0047057b8, 0xfffffff0047057dc):
        w = word_at(va)
        check(w == 0x14000004, f'AMFI CT-policy bypass patched at {va:#x}',
              'missing -> GPU plugin rejected, ~3 fps' if w != 0x14000004 else '')

    dsi = p.find(b'SWEET DSI: brightness')
    check(dsi > 0, 'DSI brightness driver compiled in')

    print()
    print('final_check:', 'ALL PASS' if ok else 'FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
