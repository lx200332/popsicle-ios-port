#!/usr/bin/env python3
"""Produce the trust cache DarwinBoot loads, from the IPSW, without macOS.

`unixnu build payload --trust-cache FILE` copies FILE to `Payloads/kernel.macho.trustcache`, and
`payload.py:105` refuses any file whose sha256 is not listed in `board.toml [payload]
trust_cache_sha256`. Neither of the two files the IPSW ships is on that list, because the list holds
the hash of a *bundle* of two version-2 modules, not of either file:

    bundle = <version=2, count=2, total=12+len(static)> + static_module + system_module

Both shipped files are DER envelopes around those modules (`build-original-system-trust.py` unwraps
them the same way), so this runs anywhere — no `compression_tool`, no hdiutil, no mounted DMG.

    StaticTrustCache          -> DER field 3 -> 59496-byte module, sha256 da88460e...
    Cryptex1,SystemTrustCache -> DER field 3 ->  1848-byte module, sha256 d121ba89...
    bundle                                   -> sha256 7416ca4f... (the first pin)

Run it after `unixnu ipsw fetch StaticTrustCache 'Cryptex1,SystemTrustCache'`; the default input
directory is `out/ipsw/<build>/`.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

MODULE_VERSION = 2
ENTRY_BYTES = 24
HEADER_BYTES = 24
BUNDLE_HEADER_BYTES = 12

# The two pins sheng accepts, so a mismatch is reported as "not the pinned bundle" rather than as a
# bare hash the reader has to compare by eye. Overridable with --expect.
PINS = (
    '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7',
    '8900d6a93d7a59ce1a872a5c081e559f4240a218f97e12d44d22dda46644d68f',
)


def der(data, at):
    """Read one DER TLV, the way build-original-system-trust.py does."""
    if at + 2 > len(data):
        raise ValueError('truncated DER')
    tag, size = data[at], data[at + 1]
    at += 2
    if size & 0x80:
        count = size & 0x7F
        if not 0 < count <= 4 or at + count > len(data):
            raise ValueError('invalid DER length')
        size = int.from_bytes(data[at:at + count], 'big')
        at += count
    if at + size > len(data):
        raise ValueError('DER value runs past the end of the input')
    return tag, data[at:at + size], at + size


def module_of(blob, name):
    """The version-2 trust module inside one shipped .trustcache file."""
    tag, sequence, end = der(blob, 0)
    fields, cursor = [], 0
    while cursor < len(sequence):
        kind, data, cursor = der(sequence, cursor)
        fields.append((kind, data))
    if tag != 0x30 or end != len(blob) or [kind for kind, _ in fields] != [0x16, 0x16, 0x16, 4]:
        raise SystemExit(f'{name}: not a four-field DER envelope around an IM4P payload')
    if fields[0][1] != b'IM4P':
        raise SystemExit(f'{name}: the first DER string is not IM4P')
    module = fields[3][1]
    version, = struct.unpack_from('<I', module, 0)
    count, = struct.unpack_from('<I', module, 20)
    expected = HEADER_BYTES + count * ENTRY_BYTES
    if version != MODULE_VERSION:
        raise SystemExit(f'{name}: module version {version}, expected {MODULE_VERSION}')
    if len(module) != expected:
        raise SystemExit(f'{name}: {len(module)} bytes but {count} entries need {expected}')
    return module, count


def locate(input_dir):
    """Find the two .trustcache files under `input_dir`.

    ipsw.py caches members under out/ipsw/<build>/, and <build> changes with board.toml [ipsw], so
    accepting either that directory or its parent keeps the workflow free of a hardcoded build id.
    A parent with several build directories is an error rather than a guess.
    """
    names = ('044-76040-038.dmg.trustcache', '044-76034-038.dmg.trustcache')
    if all((input_dir / name).is_file() for name in names):
        return input_dir, names
    nested = sorted({path.parent for name in names for path in input_dir.glob(f'*/{name}')})
    if len(nested) == 1 and all((nested[0] / name).is_file() for name in names):
        return nested[0], names
    if not nested:
        raise SystemExit(f'no {names[0]} under {input_dir}: run "unixnu ipsw fetch StaticTrustCache '
                         f"'Cryptex1,SystemTrustCache'\" first")
    raise SystemExit(f'{len(nested)} build directories under {input_dir}; pass --input with one of: '
                     + ', '.join(str(path) for path in nested))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--input', type=Path,
                        default=Path('out/ipsw'),
                        help='the ipsw cache, or one build directory inside it (default: %(default)s)')
    parser.add_argument('--output', type=Path,
                        default=Path('build/popsicle/boot/kernel.macho.trustcache'),
                        help='bundle to write (default: %(default)s)')
    parser.add_argument('--expect', action='append', default=None,
                        help='acceptable sha256 of the bundle (repeatable; default: the two board pins)')
    parser.add_argument('--json', type=Path, default=None,
                        help='also write the derivation record here')
    args = parser.parse_args()

    directory, names = locate(args.input)
    static_path, system_path = directory / names[0], directory / names[1]

    static, static_count = module_of(static_path.read_bytes(), static_path.name)
    system, system_count = module_of(system_path.read_bytes(), system_path.name)

    # The three fields are version, header size and the size of the static region. The literal 12 is
    # copied verbatim from build-original-system-trust.py:53 — it is a header-size field, not a module
    # count (a count would be 2), and reproducing it is what makes the bundle hash to the pinned value.
    header = struct.pack('<3I', MODULE_VERSION, BUNDLE_HEADER_BYTES, BUNDLE_HEADER_BYTES + len(static))
    bundle = header + static + system
    digest = hashlib.sha256(bundle).hexdigest()

    accepted = tuple(args.expect) if args.expect else PINS
    if digest not in accepted:
        raise SystemExit(f'the bundle hashes to {digest}, which is not one of the accepted pins '
                         f'({", ".join(accepted)}); the IPSW build is not the one board.toml was '
                         f'qualified against')

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bundle)

    record = {
        'static_trustcache': {'file': static_path.name, 'module_bytes': len(static),
                              'entries': static_count, 'module_sha256': hashlib.sha256(static).hexdigest()},
        'system_trustcache': {'file': system_path.name, 'module_bytes': len(system),
                              'entries': system_count, 'module_sha256': hashlib.sha256(system).hexdigest()},
        'bundle': {'bytes': len(bundle), 'modules': 2,
                   'entries': static_count + system_count,
                   'sha256': digest, 'path': str(args.output)},
    }
    if args.json:
        import json
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(record, indent=2) + '\n')

    print(f'static module {len(static)} bytes / {static_count} entries  {record["static_trustcache"]["module_sha256"]}')
    print(f'system module {len(system)} bytes / {system_count} entries  {record["system_trustcache"]["module_sha256"]}')
    print(f'bundle        {len(bundle)} bytes / 2 modules / {static_count + system_count} entries  {digest}')
    print(args.output)
    return 0


if __name__ == '__main__':
    sys.exit(main())
