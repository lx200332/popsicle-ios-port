#!/bin/bash
# Run the popsicle preflight with explicit paths, and dump the ADT with the real record layout.
set -e
DEV=<BUILD>/UniXNU-popsicle
UEFI=<BUILD>/mu-silicium

echo "=== preflight: check-popsicle.py ==="
UNIXNU_UEFI="$UEFI" python3 "$DEV/scripts/check-popsicle.py" 2>&1 | tail -20

echo
echo "=== ADT structure dump (little-endian, 32-byte property names) ==="
python3 - /tmp/popsicle-test.adt <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()

def walk(off, depth, out):
    nprop, nchild = struct.unpack_from('<II', d, off)
    off += 8
    name = '?'
    for i in range(nprop):
        raw, ln = struct.unpack_from('<32sI', d, off)
        off += 36
        nm = raw.split(b'\0')[0].decode('latin1')
        val = d[off:off+ln]
        off += (ln + 3) & ~3
        if i == 0:
            name = val.split(b'\0')[0].decode('latin1')
        else:
            out.append((depth, name, nm, val))
    for _ in range(nchild):
        off = walk(off, depth + 1, out)
    return off

props = []
walk(0, 0, props)
print(f'{len(props)} properties, max depth {max(p[0] for p in props)}')
for depth, node, nm, val in props:
    show = val
    if len(val) <= 48:
        try:
            s = val.rstrip(b'\0').decode('ascii')
            if all(32 <= ord(c) < 127 for c in s):
                show = s
        except Exception:
            pass
    elif nm in ('reg', 'ranges'):
        show = ' '.join(hex(x) for x in struct.unpack_from('<%dI' % (len(val)//4), val))
    else:
        show = f'<{len(val)} bytes>'
    print(f'{"  " * depth}{node}.{nm} = {show}')
PY
