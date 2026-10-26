#!/usr/bin/env python3
"""Prove that patches/ applies to the edk2 revision unixnu.lock pins, without a full edk2 checkout.

Why this exists
---------------
`unixnu build firmware` fetches its own `mu_basecore` at the revision in `unixnu.lock` and runs
`git apply` for every entry of `[firmware] patches` (firmware.py:56-59). If one of them does not
apply, the runner fails several minutes in, after the whole Mu-Silicium clone. That is a slow way to
learn that a patch list is wrong, and this machine cannot complete the fetch that would let the real
thing be exercised here (`codeload` truncates the 7 MB tarball well before the end; see
KERNEL-BUILD.md).

The patches touch eleven files in total. Fetching exactly those eleven at the pin, through the
contents API, is a few hundred kilobytes, and `git apply --check` against them answers the same
question the runner asks. What this does *not* prove is that nothing else in edk2 changed between
the pin and the checkout the build actually gets: it is a proof about the patch targets, not about
the whole tree.

The other half of the same question is answered from the build itself. The tree this board was
built and verified against is `release/202608` at 2026-10-23, five patches ahead of the pin, and
`build_uefi.py` manages those five by reverse-then-reapply (build_uefi.py:491-514). A firmware
built there, with Gic-Spurious staged, therefore compiles pin+six. `scripts/explain-firmware-parity.py`
lays that out line by line.
"""
import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / 'unixnu.lock'
PATCHES = ROOT / 'patches'
# Raw file service, not the contents API. The API is rate limited to 60 requests an hour per
# unauthenticated address and this check needs eleven; a runner that has already spent its quota on
# something else would fail here for a reason that has nothing to do with the patches. The raw host
# serves the same bytes at a pinned commit without counting against that budget.
BASE = 'https://raw.githubusercontent.com/microsoft/mu_basecore/{}/{}'
# The one refer pin that is edk2. A missing or renamed entry is an error, not a silent skip.
EDK2 = 'edk2'


def pin():
    """Read the edk2 revision out of unixnu.lock.

    The file is TOML with one `[source."name"]` section per resolved git feed and a `commit` key
    inside it (unixnu.lock is written by `unixnu fetch`, see feeds.py:lock_write). Parsed by hand
    rather than with tomllib so this runs on any python3 the runner happens to have.
    """
    text = LOCK.read_text(encoding='utf-8')
    current, entry = None, {}
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith('[') and stripped.endswith(']'):
            if current == EDK2 and entry.get('commit'):
                return entry['commit']
            header = stripped[1:-1].strip()
            current = None
            match = re.fullmatch(r'source\s*\.\s*"([^"]+)"', header)
            if match:
                current = match.group(1)
            entry = {}
            continue
        if current is None or '=' not in stripped:
            continue
        key, _, value = stripped.partition('=')
        entry[key.strip()] = value.strip().strip('"')
    if current == EDK2 and entry.get('commit'):
        return entry['commit']
    raise SystemExit(f'unixnu.lock: no [source."{EDK2}"] section with a commit')


def targets():
    """Every path the patches rewrite, in patch order, de-duplicated."""
    seen, order = set(), []
    for patch in sorted(PATCHES.glob('*.patch')):
        for line in patch.read_text(encoding='utf-8', errors='replace').splitlines():
            if line.startswith('+++ b/'):
                path = line[6:].strip()
                if path not in seen:
                    seen.add(path)
                    order.append(path)
    if not order:
        raise SystemExit(f'{PATCHES}: no patch names a target file')
    return order


def fetch(path, rev, into, attempts=6):
    """One file at the pin, as the bytes GitHub serves at that commit.

    Retried with a growing delay: a dropped TLS connection says nothing about the patch. A file that
    is still missing after every attempt is a real error and stops the run, because checking a patch
    against a partial tree would be meaningless.

    A 404 is not retried. At a pinned commit it means the path is wrong, which no amount of waiting
    fixes, and saying so immediately is more useful than six attempts.
    """
    destination = into / path
    destination.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(BASE.format(rev, path), headers={'User-Agent': 'check-patches-at-pin'})
    last = None
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(request, timeout=90) as response:
                body = response.read()
            if not body:
                raise ValueError('empty response')
            destination.write_bytes(body)
            return len(body)
        except urllib.error.HTTPError as error:
            if error.code == 404:
                raise SystemExit(f'{path} does not exist at {rev}: the patch names a path this '
                                 'revision does not have')
            last = error
        except Exception as error:                              # noqa: BLE001 - reported verbatim
            last = error
        if attempt < attempts:
            time.sleep(min(2 ** attempt, 20))
    raise SystemExit(f'{path} at {rev}: {last}')


def main():
    rev = pin()
    files = targets()
    into = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/pin-check')
    into.mkdir(parents=True, exist_ok=True)
    print(f'edk2 pin  {rev}')
    print(f'targets    {len(files)} file(s)')
    total = 0
    for path in files:
        total += fetch(path, rev, into)
        print(f'  fetched  {path}')
    print(f'           {total} bytes')

    subprocess.run(['git', 'init', '-q', '.'], cwd=into, check=True)
    subprocess.run(['git', 'add', '-A', '-f'], cwd=into, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['git', '-c', 'user.email=pin@check', '-c', 'user.name=pin', 'commit', '-qm', 'pin'],
                   cwd=into, check=True, stdout=subprocess.DEVNULL)

    failed = []
    for patch in sorted(PATCHES.glob('*.patch')):
        # The runner applies each patch in turn, so each check has to see the ones before it. Applying
        # for real (not only --check) keeps the sequence honest; the copy under build/ is throwaway.
        check = subprocess.run(['git', 'apply', '--check', str(patch)], cwd=into, capture_output=True, text=True)
        if check.returncode != 0:
            failed.append(patch.name)
            print(f'  {patch.stem:<14} DOES NOT APPLY  {check.stderr.strip().splitlines()[:1]}')
            continue
        applied = subprocess.run(['git', 'apply', str(patch)], cwd=into, capture_output=True, text=True)
        if applied.returncode != 0:
            failed.append(patch.name)
            print(f'  {patch.stem:<14} FAILED TO APPLY  {applied.stderr.strip().splitlines()[:1]}')
            continue
        print(f'  {patch.stem:<14} applies')

    result = {'edk2_pin': rev, 'targets': files, 'failed': failed,
              'patches': sorted(p.stem for p in PATCHES.glob('*.patch'))}
    (into / 'check.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print()
    if failed:
        print(f'{len(failed)} of {len(result["patches"])} patch(es) do not apply at the pin: {", ".join(failed)}')
        return 1
    print(f'all {len(result["patches"])} patch(es) apply at the pin')
    return 0


if __name__ == '__main__':
    sys.exit(main())
