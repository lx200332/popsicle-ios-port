"""Read back an ADT that make-popsicle-adt.py emitted and check its structure.

A generator with no reader is untested: it can emit bytes that parse as nothing and
still "succeed". This decodes the Apple flat device tree format from scratch and
asserts the properties the board asks for, so a layout mistake in the generator
(wrong name field width, wrong alignment, wrong cells) fails here instead of on the
handset.

The format, as make-popsicle-adt.py writes it and as this reads it:

    node  := uint32 nprops, uint32 nchildren, property*, child*
    prop  := char name[32], uint32 length, uint8 data[length], pad to 4

The root node's first property is always `name`; its value here is `device-tree`,
not `/`. Child nodes follow the properties, in order, nchildren of them.

**The whole tree is little-endian.** Cell and quad payloads are native `<I` / `<Q`,
not the big-endian cells a Devicetree-specification tree would use. Decoding them as
big-endian produces numbers that look plausible (a six-cell `ranges`, a four-cell
`reg`) but are wrong, which is exactly the kind of mistake that survives review. The
`chosen/memory-map` node also carries its entries as *properties* (`PackedRAMDisk`,
`TrustCache`, `GpuAddon`), not as children.

Usage: verify-adt.py ADT [--dram-size HEX] [--cpus N] [--chip-id HEX]
                        [--require-sidecar NAME ...]

--require-sidecar names a handoff the caller knows must be present (PackedRAMDisk,
TrustCache, GpuAddon). Without it an empty memory-map passes, because a payload with no
trust cache, no GPU addon and no Data seed legitimately hands nothing to the loader.
"""
import struct
import sys

NAME_FIELD = 32


class Node:
    def __init__(self, name):
        self.name = name
        self.props = {}
        self.children = []

    def child(self, name):
        for c in self.children:
            if c.name == name:
                return c
        return None

    def walk(self, depth=0):
        yield self
        for c in self.children:
            yield from c.walk(depth + 1)


def read_node(data, at):
    if at + 8 > len(data):
        raise ValueError(f'truncated node header at {at:#x}')
    nprops, nchildren = struct.unpack_from('<II', data, at)
    at += 8
    if nprops > 4096 or nchildren > 4096:
        raise ValueError(f'implausible node at {at - 8:#x}: {nprops} props, {nchildren} children')
    node = Node(None)
    for _ in range(nprops):
        if at + NAME_FIELD + 4 > len(data):
            raise ValueError(f'truncated property header at {at:#x}')
        raw = data[at:at + NAME_FIELD]
        name = raw.split(b'\0')[0].decode('ascii', 'replace')
        length = struct.unpack_from('<I', data, at + NAME_FIELD)[0]
        at += NAME_FIELD + 4
        if at + length > len(data):
            raise ValueError(f'property {name} runs past the end ({length} bytes at {at:#x})')
        node.props[name] = data[at:at + length]
        at += length
        at += (-length) % 4
    if 'name' in node.props:
        node.name = node.props['name'].split(b'\0')[0].decode('ascii', 'replace')
    for _ in range(nchildren):
        kid, at = read_node(data, at)
        node.children.append(kid)
    return node, at


def cells(value):
    """Cell values, little-endian, the way the generator writes them."""
    return list(struct.unpack(f'<{len(value) // 4}I', value)) if value else []


def quads(value):
    return list(struct.unpack(f'<{len(value) // 8}Q', value)) if value else []


def main(argv):
    # --help has to be honoured before argv[0] is taken as a path, otherwise asking a
    # checker how to use it reports "No such file or directory: '--help'".
    if not argv or argv[0] in ('-h', '--help'):
        raise SystemExit(__doc__)
    path = argv[0]
    want_dram = None
    want_cpus = None
    want_chip = None
    required_sidecars = []
    i = 1
    while i < len(argv):
        if argv[i] == '--dram-size':
            want_dram = int(argv[i + 1], 0)
            i += 2
        elif argv[i] == '--cpus':
            want_cpus = int(argv[i + 1], 0)
            i += 2
        elif argv[i] == '--chip-id':
            want_chip = int(argv[i + 1], 0)
            i += 2
        elif argv[i] == '--require-sidecar':
            # The name the loader uses: PackedRAMDisk, TrustCache, GpuAddon.
            required_sidecars.append(argv[i + 1])
            i += 2
        else:
            raise SystemExit(f'unknown argument {argv[i]}')

    data = open(path, 'rb').read()
    try:
        root, end = read_node(data, 0)
    except ValueError as exc:
        # A malformed tree is a finding, not a crash: report it the same way every
        # other structural problem is reported.
        print(f'adt {path}')
        print(f'  {len(data)} bytes')
        print(f'  FAIL  the tree does not decode: {exc}')
        print()
        print('1 structural check(s) failed')
        return 1
    trailing = len(data) - end
    nodes = list(root.walk())

    failures = 0

    def check(ok, good, bad):
        nonlocal failures
        print(('  ok    ' if ok else '  FAIL  ') + (good if ok else bad))
        if not ok:
            failures += 1

    print(f'adt {path}')
    print(f'  {len(data)} bytes, {len(nodes)} nodes, {trailing} trailing bytes')
    check(trailing == 0, 'the tree consumes the whole file',
          f'{trailing} bytes are unaccounted for, so the layout is wrong')
    check(root.name == 'device-tree',
          f'root node is named "device-tree" (got {root.name!r})',
          f'root node is {root.name!r}, expected "device-tree"')

    for prop in ('compatible', 'model', 'target-type', '#address-cells', '#size-cells'):
        check(prop in root.props, f'root carries {prop}', f'root is missing {prop}')

    compat = root.props.get('compatible', b'')
    parts = [p.decode() for p in compat.split(b'\0') if p]
    check(any(p.startswith('xiaomi,') for p in parts),
          f'compatible names this board: {parts}', f'compatible does not name the board: {parts}')
    check(any(p.startswith('qcom,') for p in parts),
          'compatible names the SoC family', f'compatible has no qcom entry: {parts}')

    chosen = root.child('chosen')
    if chosen is None:
        print('  FAIL  no chosen node')
        failures += 1
    else:
        for prop in ('random-seed', 'osenvironment', 'darwinboot-memory-size',
                     'dram-base', 'dram-size', 'crypto-hash-method'):
            check(prop in chosen.props, f'chosen carries {prop}', f'chosen is missing {prop}')
        seed = chosen.props.get('random-seed', b'')
        check(len(seed) == 64, f'random-seed is 64 bytes ({len(seed)})',
              f'random-seed is {len(seed)} bytes, expected 64')
        check(chosen.props.get('osenvironment', b'').rstrip(b'\0') == b'main',
              'osenvironment is "main"', 'osenvironment is not "main"')
        if want_dram is not None:
            got = struct.unpack('<Q', chosen.props.get('dram-size', b'\0' * 8))[0]
            check(got == want_dram, f'dram-size is {hex(got)} as requested',
                  f'dram-size is {hex(got)}, expected {hex(want_dram)}')
        nv = chosen.props.get('nvram-proxy-data', b'')
        check(len(nv) > 0 and len(nv) % 16 == 0,
              f'nvram-proxy-data is {len(nv)} bytes and 16-byte aligned',
              f'nvram-proxy-data is {len(nv)} bytes')
        if want_chip is not None:
            got = struct.unpack('<I', chosen.props.get('chip-id', b'\0' * 4))[0]
            check(got == want_chip, f'chip-id is {hex(got)} as requested',
                  f'chip-id is {hex(got)}, expected {hex(want_chip)}')
        unique = chosen.props.get('unique-chip-id', b'')
        check(len(unique) == 8, f'unique-chip-id is {len(unique)} bytes',
              f'unique-chip-id is {len(unique)} bytes, expected 8')
        memmap = chosen.child('memory-map')
        if memmap is None:
            print('  FAIL  chosen has no memory-map child')
            failures += 1
        else:
            # These are properties of memory-map, not children of it: each is the
            # 16-byte sidecar handoff the loader fills in.
            handed = [k for k in memmap.props if k != 'name']
            # An empty memory-map is legitimate: a payload built with no trust cache, no
            # GPU addon and no Data seed gives the loader nothing to hand over, so the
            # generator correctly declares nothing. Only a caller that names the sidecars
            # it expects can hold this to a minimum, which is what --require-sidecar is
            # for. Failing unconditionally here would reject a valid tree.
            if required_sidecars:
                missing = [name for name in required_sidecars if name not in handed]
                check(not missing, f'memory-map declares {sorted(handed)}',
                      f'memory-map is missing the requested handoff(s) {missing}: '
                      f'it declares {sorted(handed) or "none"}')
            elif handed:
                print(f'  ok    memory-map declares {sorted(handed)}')
            else:
                print('  note  memory-map declares no handoffs (no sidecar was requested)')
            for name, value in memmap.props.items():
                if name == 'name':
                    continue
                check(len(value) == 16,
                      f'{name} is a 16-byte handoff',
                      f'{name} is {len(value)} bytes, expected 16')

    cpus = root.child('cpus')
    if cpus is None:
        print('  FAIL  no cpus node')
        failures += 1
    else:
        cores = [c for c in cpus.children if c.name.startswith('cpu')]
        check(bool(cores), f'cpus declares {len(cores)} core(s)',
              'cpus declares no cores')
        regs = [struct.unpack('<I', c.props['reg'])[0] for c in cores if 'reg' in c.props]
        check(len(regs) == len(cores), 'every core has a reg',
              f'{len(cores) - len(regs)} core(s) have no reg')
        check(len(set(regs)) == len(regs), f'core regs are distinct: {[hex(r) for r in regs]}',
              f'core regs repeat: {[hex(r) for r in regs]}')
        check(regs[:1] == [0], 'the first core is reg 0', f'the first core is reg {regs[:1]}')
        if want_cpus is not None:
            check(len(cores) == want_cpus, f'{want_cpus} core(s) as requested',
                  f'{len(cores)} core(s), expected {want_cpus}')
        states = [c.props.get('state', b'').rstrip(b'\0').decode() for c in cores]
        check(states.count('running') == 1,
              'exactly one core is marked running',
              f'{states.count("running")} cores are marked running')
        psci = root.child('psci')
        if len(cores) > 1:
            check(psci is not None, 'a multi-core tree declares psci', 'a multi-core tree has no psci')
        if psci is not None:
            method = psci.props.get('method', b'').rstrip(b'\0')
            check(method in (b'smc', b'hvc'), f'psci method is {method.decode()}',
                  f'psci method is {method!r}')

    armio = root.child('arm-io')
    if armio is None:
        print('  FAIL  no arm-io node')
        failures += 1
    else:
        # ranges is three quads: child address, parent address, size.
        ranges = quads(armio.props.get('ranges', b''))
        check(len(ranges) == 3, f'arm-io ranges is {len(ranges)} quads',
              f'arm-io ranges is {len(ranges)} quads, expected 3')
        window = ranges[2] if len(ranges) > 2 else 0
        base = ranges[1] if len(ranges) > 1 else 0
        check(window > 0, f'arm-io window is {hex(window)} at {hex(base)}',
              'arm-io window is empty, so every MMIO child would be unreachable')
        intc = armio.child('intc')
        if intc is None:
            print('  FAIL  arm-io has no intc')
            failures += 1
        else:
            # reg is four quads: distributor address+size, redistributor address+size.
            reg = quads(intc.props.get('reg', b''))
            check(len(reg) >= 4, f'intc reg is {[hex(r) for r in reg]}',
                  f'intc reg is {len(reg)} quads, expected 4')
            if len(reg) >= 4:
                dist, dist_size, redist, redist_size = reg[:4]
                check(dist_size > 0 and redist_size > 0,
                      f'distributor {hex(dist)}+{hex(dist_size)}, '
                      f'redistributor {hex(redist)}+{hex(redist_size)}',
                      'one of the GIC windows is empty')
                check(dist < base + window and redist < base + window,
                      'both GIC windows sit inside the arm-io mapping',
                      f'a GIC window ({hex(dist)} / {hex(redist)}) falls outside '
                      f'arm-io {hex(base)}..{hex(base + window)}')
                check(redist >= dist + dist_size,
                      'the redistributor starts after the distributor',
                      f'the redistributor at {hex(redist)} overlaps the distributor '
                      f'ending at {hex(dist + dist_size)}')
            stride = cells(intc.props.get('redistributor-stride', b''))
            check(bool(stride) and stride[0] > 0,
                  f'redistributor-stride is {hex(stride[0]) if stride else "absent"}',
                  'redistributor-stride is missing or zero')
        ufshc = armio.child('ufshc')
        check(ufshc is not None, 'arm-io declares ufshc', 'arm-io has no ufshc')

    print()
    if failures:
        print(f'{failures} structural check(s) failed')
        return 1
    print('the ADT decodes as a well-formed Apple device tree with the properties the board asks for')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
