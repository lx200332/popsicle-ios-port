#!/usr/bin/env python3
"""Put this repository's board patches where the firmware build looks for them.

unixnu's firmware step resolves a patch as `uefi / patch`, i.e. relative to the Mu-Silicium
checkout, and applies it with `git -C <edk2> apply`. That is the right place for a patch against
the shared EDK II sources, but it means the file has to be inside a checkout this repository does
not own -- and the build clones Mu-Silicium fresh, so a patch committed here is invisible to it.

This runs as a preflight step, which firmware.py executes before it applies anything, and copies
`patches/*.patch` into `<uefi>/Resources/MuPatches/`. It is idempotent: an identical file is left
alone, so a second run does not touch the checkout's mtime.

Nothing is staged blindly. A patch is only copied if `git apply --check` accepts it against the
checkout it is aimed at, and an already-applied patch is reported and skipped rather than left to
fail the build later:

    already applied   the checkout carries the change; copying it would make git apply refuse
    applies cleanly   staged, and firmware.py will apply it
    neither           reported as an error, because the checkout is not the one this was written for
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def find_uefi():
    """The Mu-Silicium checkout, by the same rules check-popsicle.py uses."""
    explicit = os.environ.get('UNIXNU_UEFI')
    if explicit:
        path = Path(explicit)
        if not (path / 'Resources/Configs/popsicle.toml').is_file():
            raise SystemExit(f'UNIXNU_UEFI={explicit} is not a Mu-Silicium checkout '
                             f'carrying Resources/Configs/popsicle.toml')
        return path
    for candidate in (Path('.cache/firmware'), Path('mu-silicium'), Path('.')):
        if (candidate / 'Resources/Configs/popsicle.toml').is_file():
            return candidate.resolve()
    raise SystemExit('no Mu-Silicium checkout here: run from the assembled tree, from inside the '
                     'checkout, or set UNIXNU_UEFI to it')


def git(*args):
    return subprocess.run(['git', *map(str, args)], capture_output=True, text=True)


def find_edk2(uefi):
    """The tree git apply runs against. firmware.py passes the edk2 refer module; fall back to the
    checkout's own Mu_Basecore when this is a local build that has no refer checkout."""
    explicit = os.environ.get('UNIXNU_EDK2')
    if explicit:
        return Path(explicit)
    for candidate in (uefi / '.cache/edk2', uefi / 'Mu_Basecore', uefi):
        if (candidate / 'ArmPkg').is_dir():
            return candidate
    return uefi


def main():
    uefi = find_uefi()
    edk2 = find_edk2(uefi)
    target = uefi / 'Resources/MuPatches'
    target.mkdir(parents=True, exist_ok=True)

    patches = sorted((REPO / 'patches').glob('*.patch'))
    if not patches:
        print('no board patches to stage')
        return 0

    staged, present, skipped, failed = [], [], [], []
    for patch in patches:
        if git('-C', edk2, 'apply', '--check', patch).returncode == 0:
            destination = target / patch.name
            if destination.is_file() and destination.read_bytes() == patch.read_bytes():
                present.append(patch.name)
            else:
                shutil.copy2(patch, destination)
                staged.append(patch.name)
        elif git('-C', edk2, 'apply', '--check', '--reverse', patch).returncode == 0:
            skipped.append(patch.name)
        else:
            message = git('-C', edk2, 'apply', '--check', patch).stderr.strip().splitlines()
            failed.append((patch.name, message[0] if message else 'does not apply'))

    for name in staged:
        print(f'staged  {name} -> {target.relative_to(uefi)}')
    for name in present:
        print(f'present {name}')
    for name in skipped:
        print(f'note: {name} is already applied in {edk2.name}')
    for name, why in failed:
        print(f'error: {name}: {why}')
    print(f'{len(staged)} staged, {len(present)} already present, '
          f'{len(skipped)} already applied, {len(failed)} unusable')

    # "already applied" is a normal state for a checkout that was patched by hand, and it is the state
    # `build_uefi.py` leaves behind (it reverse-applies and re-applies five of these itself). It is not
    # something this script should veto: only a patch that neither applies nor is present means the
    # checkout is not the one these patches were written for, and that is a real error.
    #
    # It does mean `unixnu build firmware` will stop on it, because firmware.py:58 runs
    # `git apply --check` and its revert in the finally block is unconditional. Point at the fix
    # rather than failing the preflight, which also runs for the kernel-only steps.
    if skipped:
        print()
        print('Those patches are already in the edk2 checkout. That is fine for a local build, but')
        print('`unixnu build firmware` checks with `git apply --check` first and will stop: revert them')
        print('(`git -C <edk2> apply --reverse <patch>`) before a firmware build. A fresh clone of the')
        print('unixnu.lock pin is not affected.')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
