"""Check board.toml's [kernel.prelink] early_security_init against the real kernelcache.

The value is inherited from the sheng board, and until now it was inherited on the
argument that both boards fetch the same IPSW. That argument is good but it is not
evidence. This script turns it into evidence: it opens the kernelcache that has
already been proved to hash to the pin, walks the prelinked kexts, and applies the
constraints emit_prelink.py enforces on --early-security-init:

  1. the named bundle exists and carries AppleSecurityExtension = true
  2. the address is 4-byte aligned
  3. the address lands inside that bundle's __TEXT_EXEC, whose initprot is 5
     (read+execute) and whose maxprot does not include write
  4. the address after the board's slide does not wrap

Two things about the on-disk format are easy to get wrong and are handled here:

  * __PRELINK_INFO holds a bare <dict>...</dict> with no <?xml declaration and no
    <plist> wrapper, so the envelope has to be added back before parsing.
  * The plist is Apple's reference-encoded flavour: the first occurrence of a value
    carries <string ID="100">text</string> and every later use is the empty
    <string IDREF="100"/>. plistlib rejects that outright (`invalid literal for
    int() with base 10: ''` comes from an empty <integer IDREF=.../>), so the
    parser here resolves ID/IDREF itself.
  * segment_command_64 puts maxprot at +56 and initprot at +60; reading in the other
    order silently yields nsects as "maxprot".

Usage: verify-esi.py KERNELCACHE BUNDLE=ADDRESS [BUNDLE=ADDRESS ...]
                     [--slide -0x1000000] [--roots board.toml]
"""
import base64
import datetime
import struct
import sys
import xml.etree.ElementTree as ET

MACHO64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
CPU_ARM64 = 0x0100000C

APPLE_EPOCH = datetime.datetime(2001, 1, 1, tzinfo=datetime.timezone.utc)


class RefResolver:
    """Decode an Apple reference-encoded plist into plain Python objects."""

    def __init__(self, root):
        self.by_id = {}
        for element in root.iter():
            ident = element.get('ID')
            if ident is not None:
                self.by_id[ident] = element
        self.memo = {}

    def value(self, element):
        ref = element.get('IDREF')
        if ref is not None:
            if ref in self.memo:
                return self.memo[ref]
            target = self.by_id.get(ref)
            if target is None:
                return None          # dangling reference; the field is simply absent
            # Guard against a cycle before recursing.
            self.memo[ref] = None
            out = self.value(target)
            self.memo[ref] = out
            return out

        tag = element.tag
        if tag == 'key':
            # Keys carry their text inline; only values get the ID/IDREF treatment.
            return element.text or ''
        if tag == 'string':
            return element.text or ''
        if tag == 'integer':
            text = (element.text or '').strip()
            if not text:
                return 0
            # Apple writes addresses as 0x... here, and small counts in decimal.
            return int(text, 16) if text.lower().startswith(('0x', '-0x')) else int(text)
        if tag == 'real':
            text = (element.text or '').strip()
            return float(text) if text else 0.0
        if tag == 'true':
            return True
        if tag == 'false':
            return False
        if tag == 'data':
            text = (element.text or '').strip()
            return base64.b64decode(text) if text else b''
        if tag == 'date':
            text = (element.text or '').strip()
            return APPLE_EPOCH + datetime.timedelta(seconds=float(text)) if text else APPLE_EPOCH
        if tag == 'array':
            return [self.value(child) for child in element]
        if tag == 'dict':
            out = {}
            children = list(element)
            for i in range(0, len(children) - 1, 2):
                key = children[i]
                name = self.value(key)
                out[name if isinstance(name, str) else str(name)] = self.value(children[i + 1])
            return out
        return None


def segments(data, base_off):
    """name -> segment fields, for the Mach-O whose header sits at base_off."""
    magic, cpu, _, _, ncmds, _, _ = struct.unpack_from('<IiIIIII', data, base_off)
    if magic != MACHO64:
        raise ValueError(f'not a 64-bit Mach-O at {base_off:#x} (magic {magic:#x})')
    if cpu != CPU_ARM64:
        raise ValueError(f'not arm64 at {base_off:#x} (cputype {cpu:#x})')
    at = base_off + 32
    out = {}
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        if cmd == LC_SEGMENT_64:
            name = data[at + 8:at + 24].split(b'\0')[0].decode('ascii', 'replace')
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, at + 24)
            maxprot, initprot = struct.unpack_from('<ii', data, at + 56)
            out[name] = dict(vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff,
                             filesize=filesize, initprot=initprot, maxprot=maxprot)
        at += size
    return out


def prelinked_kexts(data, segs):
    """The __PRELINK_INFO dictionary, as a list of per-kext dicts."""
    seg = segs.get('__PRELINK_INFO')
    if not seg:
        raise ValueError('no __PRELINK_INFO segment')
    blob = data[seg['fileoff']:seg['fileoff'] + seg['filesize']]
    start = blob.find(b'<dict>')
    end = blob.rfind(b'</dict>')
    if start < 0 or end < 0:
        raise ValueError('no <dict> in __PRELINK_INFO')
    body = blob[start:end + len(b'</dict>')]
    # plistlib cannot read the reference encoding, so parse the XML directly.
    root = ET.fromstring(body)
    info = RefResolver(root).value(root)
    return info.get('_PrelinkInfoDictionary', [])


def locate(data, outer, segs, kext, ident):
    """The file offset of a kext's own Mach-O header."""
    load = kext.get('_PrelinkExecutableLoadAddr')
    if load is None:
        raise ValueError('no _PrelinkExecutableLoadAddr')
    if load < 0:
        load += 1 << 64
    for name in ('__PLK_TEXT_EXEC', '__PRELINK_TEXT', '__TEXT_EXEC', '__TEXT'):
        seg = segs.get(name)
        if seg and seg['vmaddr'] <= load < seg['vmaddr'] + seg['vmsize']:
            off = seg['fileoff'] + (load - seg['vmaddr'])
            if struct.unpack_from('<I', data, off)[0] == MACHO64:
                return off, load
    # Fall back to scanning for the identifier and walking backwards to a header.
    marker = ident.encode()
    at = 0
    while True:
        hit = data.find(marker, at)
        if hit < 0:
            break
        at = hit + 1
        for back in range(0, 0x200000, 4):
            if hit - back < 0:
                break
            if struct.unpack_from('<I', data, hit - back)[0] == MACHO64:
                cand = hit - back
                try:
                    inner = segments(data, cand)
                except ValueError:
                    continue
                if '__TEXT_EXEC' in inner:
                    return cand, inner['__TEXT_EXEC']['vmaddr']
    raise ValueError('cannot locate this bundle\'s Mach-O in the image')


def read_roots(path):
    """The roots list of [kernel.prelink] in a board.toml."""
    import tomllib
    from pathlib import Path
    board = tomllib.loads(Path(path).read_text())
    return board.get('kernel', {}).get('prelink', {}).get('roots', [])


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    path = argv[0]
    slide = 0
    roots_source = None
    if '--slide' in argv:
        i = argv.index('--slide')
        slide = int(argv[i + 1], 0)
        argv = argv[:i] + argv[i + 2:]
    if '--roots' in argv:
        i = argv.index('--roots')
        roots_source = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    specs = [a for a in argv[1:] if '=' in a]

    data = open(path, 'rb').read()
    outer = segments(data, 0)
    kexts = prelinked_kexts(data, outer)
    print(f'kernelcache {path}')
    print(f'  {len(data)} bytes, {len(outer)} segments, {len(kexts)} prelinked kexts')

    by_id = {}
    for k in kexts:
        ident = k.get('CFBundleIdentifier') or k.get('_PrelinkKextIdentifier')
        if ident:
            by_id[ident] = k

    failures = 0

    def check(ok, good, bad):
        nonlocal failures
        print(('  ok    ' if ok else '  FAIL  ') + (good if ok else bad))
        if not ok:
            failures += 1
        return ok

    if roots_source is not None:
        wanted = read_roots(roots_source)
        print()
        print(f'[kernel.prelink] roots: {len(wanted)} listed')
        missing = [r for r in wanted if r not in by_id]
        not_security = [r for r in wanted if r in by_id and not by_id[r].get('AppleSecurityExtension')]
        check(not missing, f'all {len(wanted)} roots are prelinked into this kernelcache',
              f'{len(missing)} root(s) are not in this kernelcache: ' + ', '.join(missing))
        if not_security:
            # Not a failure: a root need not be a security extension; only the early
            # initializer's own bundle has to be. Report it so the distinction is visible.
            print(f'  note  {len(not_security)} root(s) are not AppleSecurityExtensions: '
                  + ', '.join(not_security))

    for spec in specs:
        want, text = spec.rsplit('=', 1)
        addr = int(text, 0)
        print()
        print(f'{want} = {hex(addr)}')
        kext = by_id.get(want)
        if not check(kext is not None, 'bundle is prelinked into this kernelcache',
                     'no such bundle in this kernelcache'):
            continue
        check(bool(kext.get('AppleSecurityExtension')),
              'bundle is an AppleSecurityExtension',
              'bundle is not an AppleSecurityExtension')
        check(addr & 3 == 0, 'address is 4-byte aligned',
              f'address is not 4-byte aligned (low bits {addr & 3})')

        try:
            off, _ = locate(data, outer, outer, kext, want)
            inner = segments(data, off)
        except ValueError as exc:
            check(False, '', f'cannot read this bundle\'s Mach-O: {exc}')
            continue

        txt = inner.get('__TEXT_EXEC')
        if not check(txt is not None, 'bundle has a __TEXT_EXEC segment',
                     'bundle has no __TEXT_EXEC segment'):
            continue
        lo, hi = txt['vmaddr'], txt['vmaddr'] + txt['vmsize']
        print(f'        __TEXT_EXEC {hex(lo)}..{hex(hi)} '
              f'initprot={txt["initprot"]} maxprot={txt["maxprot"]:#x}')
        check(lo <= addr < hi,
              f'address is inside __TEXT_EXEC at +{hex(addr - lo)}',
              f'{hex(addr)} is outside __TEXT_EXEC')
        check(txt['initprot'] == 5, 'initprot is 5 (read+execute)',
              f'initprot is {txt["initprot"]}, expected 5')
        check(txt['maxprot'] & 2 == 0,
              'maxprot excludes write, so the page stays executable',
              f'maxprot {txt["maxprot"]:#x} includes write')

        shifted = (addr + slide) & 0xFFFFFFFFFFFFFFFF
        wrapped = shifted > addr if slide < 0 else shifted < addr
        check(not wrapped, f'with slide {slide:+#x} the address is {hex(shifted)}',
              f'with slide {slide:+#x} the address wraps to {hex(shifted)}')

    print()
    if failures:
        print(f'{failures} constraint(s) failed')
        return 1
    print('every constraint holds: the inherited early_security_init is correct here')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
