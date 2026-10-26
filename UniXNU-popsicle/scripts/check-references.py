#!/usr/bin/env python3
"""Check that every path board.toml and the workflow name actually resolves where it is looked up.

board.toml names files that live in four different filesystems, and putting one in the wrong place is
the failure this catches:

  repo      committed in this repository        patches/*.patch, configs/*, scripts/*, assets/*,
            and the firmware/ overlay that       firmware/**, .github/workflows/build.yml
            mirrors the checkout
  overlay   committed here, copied into the     firmware/Platforms/**  ->  Platforms/**
            checkout by supply-firmware.py       firmware/Resources/Configs/*  ->  Resources/Configs/*
                                                patches/*.patch  ->  Resources/MuPatches/
            (the dsc, the config, the six        firmware/Platforms/*/<board>Pkg/FdtBlob/*.dtb
             patches, the device tree)                            ->  Resources/DTBs/<board>.dtb
  tree      assembled by `unixnu tree` from     kits/NativePrelink/emit_prelink.py,
            the main checkout; kits/ and         drivers/**, package/**
            drivers/ are its submodules, so
            an unfetched checkout accepts
            these without tracing them
  build     only exists after a build           Build/**, build/**, out/**

A path that resolves nowhere is a typo that would otherwise surface minutes later as ENOENT from
inside a build step, after the expensive checkout and toolchain steps, reading like a compiler
problem. The empty-overlay lookups above are not hypothetical: the dsc, the patches and the device
tree all reach `firmware.py` by way of `uefi / path`, so a name committed in this repository has to be
*traceable* to that path, not merely present here.

Exit codes: 0 every reference resolves (or there was no board.toml to check from), 1 one is
unresolvable, 2 usage.
"""
import os
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent


def find_repo():
    """The directory holding board.toml.

    Normally the parent of scripts/. Inside the assembled tree it is not: `unixnu tree` creates
    <tree>/{drivers,kits,patches,refer,build,.cache,package,scripts} and copies scripts/ into it, but
    board.toml, configs/ and firmware/ stay in the device repository. A preflight invocation therefore
    has no device repository to inspect, and says so instead of reporting every path as missing."""
    candidates = [HERE.parent, pathlib.Path.cwd(), pathlib.Path.cwd().parent]
    for candidate in candidates:
        if (candidate / 'board.toml').is_file():
            return candidate
    return None


REPO = find_repo()


def find_unixnu():
    """The main UniXNU checkout, if one is reachable, so tree/ paths can be verified and not just
    accepted. `unixnu tree` links <tree>/kits/<name> to the main checkout's kits/<name> (or to this
    repository's package/ for the board's own kit), so the sources are those two directories.

    Returns None when nothing local looks like the main checkout -- a runner checks that repository
    out separately and the device repository cannot see it. classify() then reports 'tree' without
    verifying, which is the honest answer rather than a false pass or a false failure."""
    override = os.environ.get('UNIXNU_ROOT')
    candidates = [pathlib.Path(override)] if override else []
    if REPO is not None:
        candidates += [REPO.parent / 'unixnu', REPO.parent / 'unixnu-src', REPO.parent]
    for candidate in candidates:
        if (candidate / 'scripts' / 'build-xnu-4k-baseline.sh').is_file():
            return candidate
    return None


UNIXNU = find_unixnu()

# Supplied by things other than this repository. `unixnu tree` builds <tree>/{kits,drivers,patches,refer}
# as symlinks into the main checkout (or into this repository's own package/, for the board's kit), and
# copies this repository's scripts/ into <tree>/scripts.
TREE_MADE = ('kits/', 'drivers/', 'package/')
# supply-firmware.py copies these from `firmware/` into the checkout, so they are not committed at the
# checkout path either -- but their sources are, and that is what classify() traces them back to.
OVERLAY_MADE = ('Platforms/', 'BootShim/')
# Only exist after a build: the EDK II output tree and the assembled kernel.
BUILD_MADE = ('Build/', 'build/', 'out/')


def load_board():
    try:
        import tomllib
    except ModuleNotFoundError:
        import tomli as tomllib                      # type: ignore
    with (REPO / 'board.toml').open('rb') as handle:
        return tomllib.load(handle)


def references(board):
    """Every path the board description names, with where the entry came from."""
    found = []
    firmware = board['firmware']
    for entry in firmware.get('preflight', []):
        found.append(('[firmware] preflight', entry))
    for entry in firmware.get('patches', []):
        found.append(('[firmware] patches', entry))
    found.append(('[firmware] dsc', firmware['dsc']))
    found.append(('[firmware] fd', firmware['fd'].format(target='RELEASE')))
    found.append(('[firmware] config (via Resources/Configs)', f'Resources/Configs/{firmware["config"]}.toml'))
    # check-popsicle.py reads <checkout>/Resources/DTBs/<config>.dtb, which supply-firmware.py derives
    # from the overlay's FdtBlob. Naming it here is what makes a missing device tree a typo report
    # instead of an ENOENT from inside the preflight, several steps into a run.
    found.append(('[firmware] device tree (via Resources/DTBs)',
                  f'Resources/DTBs/{firmware["config"]}.dtb'))
    found.append(('[firmware.supply] overlay', firmware['supply']['overlay']))
    found.append(('[kernel] defconfig', f'configs/{board["kernel"]["defconfig"]}'))
    prelink = board['kernel'].get('prelink')
    if prelink:
        found.append(('[kernel.prelink] tool', prelink['tool']))
        found.append(('[kernel.prelink] kernel', prelink['kernel']))
    found.append(('[payload.adt] tool', board['payload']['adt']['tool']))
    return found


def workflow_references():
    path = REPO / '.github' / 'workflows' / 'build.yml'
    if not path.is_file():
        return []
    text = path.read_text(encoding='utf-8')
    return [('.github/workflows/build.yml', f'scripts/{name}')
            for name in re.findall(r'device/scripts/([A-Za-z0-9_.-]+\.py)', text)]


def tree_source(relative):
    """Trace a tree/ path back to the directory `unixnu tree` would symlink it to.

    Returns 'verified' when the source exists, 'pending' when its module directory exists but has not
    been fetched yet (kits/ and drivers/ are git submodules of the main checkout; `unixnu fetch`
    fills them, and an empty checkout is the normal state of a fresh one), False when the module
    name itself is invented, and None when no main checkout is reachable to judge by.
    """
    if UNIXNU is None:
        return None
    parts = pathlib.PurePosixPath(relative).parts
    if not parts:
        return False
    head, rest = parts[0], parts[1:]
    if head == 'package':
        base = REPO / 'package'
        target = base.joinpath(*rest) if rest else base
        return 'verified' if target.exists() else ('pending' if base.is_dir() else False)
    if head not in ('kits', 'drivers') or not rest:
        return False
    # kits/<name> is the main checkout's kits/<name> -- except the board's own kit, which is this
    # repository's package/ (tree.py links package "main" to tree/kits/<board kit>).
    own = REPO / 'package' / pathlib.PurePosixPath(*rest[1:]) if len(rest) > 1 else None
    if head == 'kits' and own is not None and own.exists():
        return 'verified'
    module = UNIXNU / head / rest[0]
    if (module / pathlib.PurePosixPath(*rest[1:])).exists():
        return 'verified'
    # Distinguish "this module is absent" from "the whole kits/ directory is absent". The latter means
    # the checkout is partial -- a shallow copy, a tarball, my own test scaffold -- and says nothing
    # about whether the name is right. Only the former makes the name itself wrong, because
    # `unixnu fetch` creates the module directory for every name the board can refer to.
    if not (UNIXNU / head).is_dir():
        return 'pending'
    return 'pending' if module.is_dir() else False


def classify(relative):
    """Where does this path actually come from: 'repo', 'overlay', 'tree' or 'build'?

    None means nothing produces it, which is the only outcome that is an error."""
    if (REPO / relative).exists():
        return 'repo'
    # Committed under a different path and copied into the checkout by supply-firmware.py: the overlay
    # mirrors the checkout, while the patch set and the device tree live outside it in this repository.
    if relative.startswith(OVERLAY_MADE):
        if (REPO / 'firmware' / relative).exists():
            return 'overlay'
        return None
    if relative.startswith('Resources/Configs/'):
        name = pathlib.PurePosixPath(relative).name
        return 'overlay' if (REPO / 'firmware/Resources/Configs' / name).is_file() else None
    if relative.startswith('Resources/MuPatches/'):
        name = pathlib.PurePosixPath(relative).name
        return 'overlay' if (REPO / 'patches' / name).is_file() else None
    if relative.startswith('Resources/DTBs/'):
        name = pathlib.PurePosixPath(relative).stem
        return 'overlay' if list((REPO / 'firmware/Platforms').glob(f'*/{name}Pkg/FdtBlob/*.dtb')) else None
    if relative.startswith(BUILD_MADE):
        return 'build'
    if relative.startswith(TREE_MADE):
        source = tree_source(relative)
        # False: the module name does not exist. None: no main checkout here, accept unverified.
        return None if source is False else 'tree'
    return None


def unverified(relative):
    """True when classify() accepted a tree/ path only because no main checkout was reachable.

    Kept separate from classify() so the summary can say how much of the pass rests on looking rather
    than on tracing, instead of implying a verification that did not happen."""
    if not relative.startswith(TREE_MADE):
        return False
    return tree_source(relative) in (None, 'pending')


def main(argv):
    if len(argv) > 1:
        print(__doc__)
        return 2
    if REPO is None:
        # Nothing to check from here, and that is not a failure: preflight runs this with cwd=<tree>,
        # where the device repository is not present. Saying so is more useful than 26 "MISS" lines.
        print(f'no board.toml under {HERE.parent} or {pathlib.Path.cwd()}: this looks like the assembled '
              f'tree, not the device repository; reference checking skipped')
        return 0
    board = load_board()
    items = references(board) + workflow_references()
    missing = []
    counts = {'repo': 0, 'overlay': 0, 'tree': 0, 'build': 0}
    unchecked = 0
    for origin, relative in items:
        where = classify(relative)
        if where is None:
            missing.append((origin, relative))
            print(f'  MISS  {relative}   ({origin})')
        else:
            counts[where] += 1
            if unverified(relative):
                unchecked += 1
    print(f'{len(items)} reference(s): {counts["repo"]} committed here, {counts["overlay"]} in the '
          f'firmware/ overlay, {counts["tree"]} produced by `unixnu tree`, {counts["build"]} by a build')
    if missing:
        print(f'{len(missing)} reference(s) resolve nowhere; the first would fail with ENOENT mid-build')
        return 1
    if unchecked:
        # Say so rather than calling an unlooked-at path verified. `unixnu fetch` populates the kit and
        # driver submodules; without that, or without a reachable main checkout, the name is assumed.
        print(f'{unchecked} tree reference(s) accepted without tracing them: no main checkout here, or '
              f'the kit submodule is not fetched yet. Set UNIXNU_ROOT to check them.')
    print('every reference resolves in the namespace it is used from')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
