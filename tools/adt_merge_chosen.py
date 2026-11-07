#!/usr/bin/env python3
"""adt_merge_chosen.py -- copy selected /chosen properties from a reference ADT
into a freshly built one.

    adt_merge_chosen.py OUT.adt REF.adt PROP [PROP ...]

mkadt_pandora.py only synthesises what it can compute; several /chosen properties
are *pinned from the running device* by an earlier pandora boot (UFS block
extents, the GPU buffer pool carve-out, the Wi-Fi MAC). They are device state,
not something to invent, so they are carried over from a known-good ADT.

Properties already present in OUT are left alone unless --force is given.
"""
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import adt


def chosen(root):
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'chosen\0':
            return c
    raise SystemExit('no /chosen')


def main(argv):
    force = '--force' in argv
    argv = [a for a in argv if a != '--force']
    if len(argv) < 3:
        raise SystemExit(__doc__)
    out_path, ref_path, names = argv[0], argv[1], argv[2:]

    root, _ = adt.parse(open(out_path, 'rb').read())
    ref, _ = adt.parse(open(ref_path, 'rb').read())
    dst, src = chosen(root), chosen(ref)
    have = dict((k, v) for k, f, v in dst.props)

    for n in names:
        key = n
        if key not in dict((k, v) for k, f, v in src.props):
            raise SystemExit(f'{ref_path}: /chosen has no {n}')
        if key in have and not force:
            print(f'keep   {n} (already present)', file=sys.stderr)
            continue
        if key in have:
            dst.props = [p for p in dst.props if p[0] != key]
        for p in src.props:
            if p[0] == key:
                dst.props.append(p)
                print(f'copy   {n} ({len(p[2])} bytes)', file=sys.stderr)
                break
    blob = adt.serialize(root)
    open(out_path, 'wb').write(blob)
    print(f'{out_path}: {len(blob)} bytes', file=sys.stderr)


if __name__ == '__main__':
    main(sys.argv[1:])
