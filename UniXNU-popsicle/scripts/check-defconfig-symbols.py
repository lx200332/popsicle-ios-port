#!/usr/bin/env python3
"""Check that every CONFIG_ symbol in the defconfig is actually read by the build.

Why this exists
---------------
`kernel.py:xnu_env()` turns each `CONFIG_XNU_FOO` of the defconfig into an `XNU_FOO` environment
variable for the legacy kernel build script (kernel.py:10-19). Nothing validates the name. A typo —
`CONFIG_XNU_NATIVE_AES` written as `CONFIG_XNU_NATIVE_AES_`, or `ARM_DT_PLATFORM` vs
`ARM_DTPLATFORM` — produces a variable no one reads, the feature silently stays at its default,
and the kernel still builds. The mistake then only shows up as a boot that behaves like a
configured-off feature, which is the most expensive kind of bug to chase on a phone.

`check-build-guards.py` answers the adjacent question ("will a guard refuse this config?"). This
one answers "is this config even connected?" The two are complements: a symbol nothing reads can
never be refused by a guard, and a guard can only fire for a symbol something reads.

What counts as "read"
---------------------
Both places that consume the configuration are searched, because they split the work:
`scripts/build-xnu-4k-baseline.sh` reads most of the flags with `${XNU_FOO:-0}`, and
`scripts/unixnu/kernel.py` reads `XNU_BUILD_ARCHIVES` to decide whether to run the archive
builders at all. Searching only the shell script reports that one as dead, which it is not.

`XNU_*` appears in the shell script for symbols that are *not* configured too — the ones with a
`:-` default, plus tags and paths. Those are listed separately as "not configured, defaults
apply", which is informational: it is how this board knows which knobs exist and are deliberately
left alone.
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFCONFIG = ROOT / 'configs' / 'popsicle_defconfig'


def consumers():
    """Locate the two files that read the configuration, in whichever tree this is running in.

    Inside the runner there is exactly one layout: `unixnu tree` assembles `out/<board>/tree/`, puts
    this repository in as `device/`, and symlinks the UniXNU feed in as `tree/scripts` (tree.py:44-69).
    Here `__file__` is under `tree/device/scripts/`, so the consumers are two directories up.

    Locally this script also runs straight out of the device repository, where the UniXNU feed is
    next to it rather than above it, and `UNIXNU_SCRIPTS` names it. Both cases have to work: the
    workflow runs this as a step, and so does anyone checking a defconfig before committing it.
    """
    candidates = []
    if os.environ.get('UNIXNU_SCRIPTS'):
        candidates.append(Path(os.environ['UNIXNU_SCRIPTS']))
    # runner: <tree>/device/scripts/this.py -> <tree>/scripts
    candidates.append(ROOT.parent / 'scripts')
    # local: the sibling checkout of the driver, or the environment variable
    candidates.append(ROOT / 'scripts')
    for folder in candidates:
        baseline = folder / 'build-xnu-4k-baseline.sh'
        kernel = folder / 'unixnu' / 'kernel.py'
        if baseline.is_file() and kernel.is_file():
            return baseline, kernel
    tried = ', '.join(str(c) for c in candidates)
    raise SystemExit('cannot find the UniXNU build scripts that read this configuration; tried '
                     f'{tried}. Set UNIXNU_SCRIPTS to the directory holding build-xnu-4k-baseline.sh.')


def configured(path):
    """The defconfig's XNU_ symbols as (name, value) in file order, without the CONFIG_ prefix."""
    if not path.is_file():
        raise SystemExit(f'{path}: no such defconfig')
    found = []
    for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith('#'):
            continue
        match = re.match(r'^(?:export\s+)?CONFIG_([A-Z0-9_]+)\s*=\s*(.*)$', stripped)
        if not match:
            raise SystemExit(f'{path}:{number}: not a CONFIG_NAME=value line: {stripped}')
        found.append((match.group(1), match.group(2).strip().strip('"')))
    if not found:
        raise SystemExit(f'{path}: no CONFIG_ symbols')
    return found


def corpus():
    """The text of every consumer, keyed by a short name for reporting."""
    texts = {}
    for path in consumers():
        texts[path.name] = path.read_text(encoding='utf-8', errors='replace')
    return texts


def main():
    symbols = configured(DEFCONFIG)
    texts = corpus()
    everything = '\n'.join(texts.values())
    read = set(re.findall(r'XNU_[A-Z0-9_]+', everything))

    dead = [(name, value) for name, value in symbols if name not in read]
    print(f'configured symbols : {len(symbols)}')
    print(f'read by the build  : {len(symbols) - len(dead)}')
    if dead:
        print()
        print('NEVER READ - a typo here is silently ignored, and the feature stays off:')
        for name, value in dead:
            print(f'  {name} = {value}')
        return 1
    print('every configured symbol is read by the build, somewhere')
    print()

    for name, value in symbols:
        where = ' '.join(key for key, text in texts.items() if name in text)
        print(f'  {name:<38} {where}')

    others = sorted(read - {name for name, _ in symbols})
    if others:
        print()
        print(f'{len(others)} XNU_ symbol(s) the build reads but this board does not configure -')
        print('defaults apply, which is deliberate unless a comment says otherwise:')
        for name in others:
            print(f'  {name}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
