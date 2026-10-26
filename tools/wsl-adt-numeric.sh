#!/bin/bash
# Decode the numeric properties of the generated popsicle ADT as integers.
set -e
python3 - /tmp/popsicle-test.adt <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
found = {}

def walk(off, depth, path, out):
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
            out[f'{path}/{name}.{nm}'] = val
    for _ in range(nchild):
        off = walk(off, depth + 1, path, out)
    return off

props = {}
walk(0, 0, '', props)

def be64(v): return [hex(x) for x in struct.unpack('>%dQ' % (len(v)//8), v)]
def be32(v): return [hex(x) for x in struct.unpack('>%dI' % (len(v)//4), v)]
def le64(v): return [hex(x) for x in struct.unpack('<%dQ' % (len(v)//8), v)]

for key in ('/chosen.dram-base', '/chosen.dram-size', '/chosen.darwinboot-memory-size',
            '/chosen.chip-id', '/chosen.unique-chip-id'):
    v = props.get(key)
    print(f'{key:42} {v.hex() if v else None}')
print()
for key in ('/arm-io.intc.reg', '/arm-io.ufshc.compatible'):
    v = props.get(key)
    if v and key.endswith('.reg'):
        print(f'{key:42} len={len(v)}')
        print('   as BE u64:', be64(v))
        print('   as LE u64:', le64(v))
    else:
        print(f'{key:42} {v}')
print()
for key in ('/cpu0.timebase-frequency', '/defaults.kern.maxvnodes', '/product.artwork-device-subtype'):
    v = props.get(key)
    print(f'{key:42} {v.hex() if v else None}  BE32={be32(v) if v else None}')
print()
print('nodes with numeric reg:')
for k, v in sorted(props.items()):
    if k.endswith('.reg'):
        print(f'  {k:36} len={len(v):3} BE64={be64(v) if len(v) % 8 == 0 else "?"}')
PY
