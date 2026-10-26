#!/usr/bin/env python3
"""Assemble the Mu-Silicium checkout the firmware step builds against.

`unixnu build firmware` needs `--uefi DIR` and `need_uefi()` refuses to continue without it: the
DERIVED board package (Platforms/Xiaomi/popsiclePkg), the device config
(Resources/Configs/popsicle.toml), the shim (BootShim/BootShim.S) and the prebuilt DXE set
(Binaries/popsicle) all live inside a Mu-Silicium checkout. None of them is upstream, so a plain
clone of Project-Silicium/Mu-Silicium cannot build this board.

This script produces that checkout deterministically:

  tarball of Mu-Silicium at the pinned commit          codeload.github.com, no clone, no history
  its submodules at the refs .gitmodules names        same mechanism, no `git submodule`
  the firmware/ tree in this repository                copied over the result, file by file
  Binaries/popsicle from Device-Binaries              sparse clone of the one device directory

then `git init` and a single commit dated with the upstream commit time. That date matters: it is
what `firmware.py` reads for SOURCE_DATE_EPOCH, and EDK II embeds a build timestamp, so the same
sources give different bytes under a different epoch. Committing with the pinned date is what makes
the runner's firmware volume byte-identical to a local one -- see the hash table in KERNEL-BUILD.md.

Everything is printed; a partial result is deleted rather than left to look complete.
"""
import configparser
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
TARBALL = 'https://codeload.github.com/{repo}/tar.gz/{rev}'


def load_board():
    try:
        import tomllib
    except ModuleNotFoundError:                     # python < 3.11
        import tomli as tomllib                     # type: ignore
    with (REPO / 'board.toml').open('rb') as handle:
        return tomllib.load(handle)


def say(message):
    print(message, flush=True)


CACHE = None


def cache_name(repo, rev):
    """One flat filename per (repo, rev). Slashes appear in both -- `release/202608` is a branch name
    -- so they are doubled rather than left to create a subdirectory."""
    return f'{repo.replace("/", "__")}@{rev.replace("/", "__")}.tar.gz'


def readable(archive):
    """True when the archive decompresses all the way to its end-of-stream marker.

    A truncated codeload download is a file of nonzero size that only fails when something reads past
    the cut -- which is minutes later, inside extract(), after the loop has already cached the bad
    bytes. Caching it turns one flaky download into a permanent failure: every later run reports the
    same EOFError and re-downloads nothing, because the cache looks warm. Reading it through here is
    the only thing that distinguishes "downloaded" from "downloaded completely"."""
    try:
        with tarfile.open(archive) as tar:
            for _ in tar:
                pass
        return True
    except (tarfile.TarError, EOFError, OSError):
        return False


def download(repo, rev, destination):
    """Fetch one tarball, reusing <cache>/<name> when it is already there and intact.

    Mu-Silicium is 68 MB and codeload serves it at a few hundred KB/s, so a cold assembly of this
    checkout takes about six minutes -- long enough that a caller will assume it hung. A cache
    directory makes the second run instant and lets an offline attempt reuse a good archive."""
    url = TARBALL.format(repo=repo, rev=rev)
    cached = None
    if CACHE is not None:
        CACHE.mkdir(parents=True, exist_ok=True)
        cached = CACHE / cache_name(repo, rev)
        if cached.is_file() and cached.stat().st_size > 0:
            if readable(cached):
                say(f'  cached {cached.name} ({cached.stat().st_size} bytes)')
                shutil.copyfile(cached, destination)
                return destination
            say(f'  discarding truncated cache entry {cached.name} ({cached.stat().st_size} bytes)')
            cached.unlink()
    say(f'  fetch {repo} @ {rev[:12]}')
    request = urllib.request.Request(url, headers={'User-Agent': 'supply-firmware'})
    with urllib.request.urlopen(request, timeout=600) as response, destination.open('wb') as out:
        shutil.copyfileobj(response, out)
    if not readable(destination):
        # Deliberately not cached: a partial archive is worth nothing and poisons the next run.
        raise SystemExit(f'the tarball for {repo} @ {rev} is truncated '
                         f'({destination.stat().st_size} bytes): codeload closed the connection '
                         f'early, so run this again')
    if cached is not None:
        shutil.copy(destination, cached)
    return destination


def extract(archive, destination):
    """Unpack, dropping codeload's <repo>-<rev>/ top directory. data filter: no escapes, no links."""
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tar:
        members = []
        for member in tar.getmembers():
            parts = Path(member.name).parts
            if len(parts) < 2:
                continue                            # the top directory itself
            member.name = str(Path(*parts[1:]))
            members.append(member)
        tar.extractall(destination, members=members, filter='data')


def submodules(root):
    """(path, url, ref) for every submodule .gitmodules declares, in file order."""
    path = root / '.gitmodules'
    if not path.is_file():
        return []
    parser = configparser.ConfigParser(delimiters=('=',), strict=False)
    parser.read(path, encoding='utf-8')
    found = []
    for section in parser.sections():
        entry = parser[section]
        name, url = entry.get('path'), entry.get('url')
        if not name or not url:
            continue
        # `branch` is a branch name here, except openssl, which pins a raw commit in it.
        found.append((name, url, entry.get('branch') or 'HEAD'))
    return found


def git(*args, **kwargs):
    return subprocess.run(['git', *map(str, args)], capture_output=True, text=True, **kwargs)


def fetch_binaries(work, spec):
    """Binaries/<device> from Device-Binaries: a sparse, blobless clone of one directory."""
    device = spec['device']
    destination = work / 'Binaries' / 'popsicle'
    if destination.is_dir():
        shutil.rmtree(destination)
    scratch = Path(tempfile.mkdtemp(prefix='devbin-'))
    try:
        say(f'  fetch {spec["repo"]} @ {spec["commit"][:12]} (sparse: {device})')
        result = git('clone', '--filter=blob:none', '--sparse', '--depth', '1',
                     f'https://github.com/{spec["repo"]}.git', scratch / 'repo')
        if result.returncode:
            raise SystemExit(f'sparse clone failed: {result.stderr.strip()}')
        result = git('-C', scratch / 'repo', 'checkout', '--detach', spec['commit'])
        if result.returncode:
            raise SystemExit(f'cannot check out {spec["commit"]}: {result.stderr.strip()}')
        result = git('-C', scratch / 'repo', 'sparse-checkout', 'set', device)
        if result.returncode:
            raise SystemExit(f'sparse-checkout {device} failed: {result.stderr.strip()}')
        source = scratch / 'repo' / device
        if not source.is_dir():
            raise SystemExit(f'{spec["repo"]} has no {device}/ directory')
        shutil.copytree(source, destination)
        say(f'    {sum(1 for _ in destination.rglob("*") if _.is_file())} files')
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def apply_overlay(work, overlay):
    """Copy overlay/ over the checkout, path for path. Its layout mirrors the checkout."""
    if not overlay.is_dir():
        return 0
    count = 0
    for source in sorted(overlay.rglob('*')):
        if not source.is_file():
            continue
        target = work / source.relative_to(overlay)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        count += 1
    return count


# Files the checkout needs at paths where this repository keeps them somewhere else. Duplicating them
# in overlay/ would put a second copy in the repository; deriving them keeps one source of truth.
#   patches/*.patch                            -> Resources/MuPatches/*.patch
#       firmware.py resolves each `patches` entry as `uefi / patch`, so they must be inside the checkout.
#   Platforms/Xiaomi/popsiclePkg/FdtBlob/*.dtb -> Resources/DTBs/<board>.dtb
#       `unixnu bootimg --dtb` takes that path explicitly, and check-popsicle.py falls back to it when
#       build/<board>/<board>.dtb is absent -- which it is until `unixnu tree` runs.
def materialize(work, board_name):
    """Copy the patch set and the merged device tree to the paths the build looks for."""
    copied = []
    patches = REPO / 'patches'
    for source in sorted(patches.glob('*.patch')):
        target = work / 'Resources' / 'MuPatches' / source.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        copied.append(f'Resources/MuPatches/{source.name}')
    tree = REPO / 'firmware/Platforms/Xiaomi' / f'{board_name}Pkg' / 'FdtBlob'
    blobs = sorted(tree.glob('*.dtb'))
    if not blobs:
        raise SystemExit(f'{tree} has no .dtb: the merged device tree is missing from the overlay')
    target = work / 'Resources' / 'DTBs' / f'{board_name}.dtb'
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(blobs[0], target)
    copied.append(f'Resources/DTBs/{board_name}.dtb')
    return copied


def main(argv):
    global CACHE
    args = list(argv)
    target = None
    force = False
    cache = REPO / '.cache' / 'tarballs'
    source_tarball = None
    while args:
        item = args.pop(0)
        if item == '--target':
            target = Path(args.pop(0))
        elif item == '--force':
            force = True
        elif item == '--cache':
            cache = Path(args.pop(0)) if args else None
        elif item == '--no-cache':
            cache = None
        elif item == '--source-tarball':
            source_tarball = Path(args.pop(0))
        elif item in ('-h', '--help'):
            print(__doc__)
            return 0
        else:
            raise SystemExit(f'unknown argument {item}')
    CACHE = cache

    board = load_board()
    supply = board.get('firmware', {}).get('supply')
    if not supply:
        raise SystemExit('board.toml has no [firmware.supply]: nothing says which Mu-Silicium to build')
    work = (target or REPO / '.cache' / 'firmware').resolve()
    if work.exists() and any(work.iterdir()):
        if not force:
            raise SystemExit(f'{work} is not empty; pass --force to rebuild it')
        shutil.rmtree(work)

    staging = Path(tempfile.mkdtemp(prefix='mupkg-'))
    try:
        if source_tarball is not None:
            # An archive that is already on this filesystem: no network, and no dependence on
            # codeload serving 68 MB at a few hundred KB/s.
            if not source_tarball.is_file():
                raise SystemExit(f'--source-tarball {source_tarball} is not a file')
            if CACHE is not None:
                CACHE.mkdir(parents=True, exist_ok=True)
                shutil.copy(source_tarball, CACHE / cache_name(supply['repo'], supply['commit']))
            archive = staging / 'mu.tar.gz'
            shutil.copy(source_tarball, archive)
            say(f'  using {source_tarball} ({archive.stat().st_size} bytes)')
        else:
            archive = download(supply['repo'], supply['commit'], staging / 'mu.tar.gz')
        say(f'  unpack into {work}')
        extract(archive, work)

        # Submodules as plain directories at the refs .gitmodules names. Mu_Basecore is deliberately
        # skipped: it is the edk2 refer module, which unixnu fetches and passes as --edk2, and the
        # checkout's own copy would only be a second, possibly different, tree.
        for index, (path, url, ref) in enumerate(submodules(work)):
            if path == 'Mu_Basecore':
                say(f'  skip {path} (edk2 comes from the refer feed / --edk2)')
                continue
            if path == 'Binaries':
                say(f'  skip {path} (filled from Device-Binaries below)')
                continue
            name = url.removesuffix('.git').split('github.com/')[-1]
            archive = download(name, ref, staging / f'sub{index}.tar.gz')
            extract(archive, work / path)
            say(f'    -> {path} ({sum(1 for _ in (work / path).rglob("*") if _.is_file())} files)')

        # Leaving .gitmodules behind would make firmware.py's `git submodule update --init` try to
        # clone over the directories just created, so the checkout declares no submodules at all.
        (work / '.gitmodules').unlink(missing_ok=True)

        if supply.get('binaries'):
            fetch_binaries(work, supply['binaries'])

        copied = apply_overlay(work, REPO / supply.get('overlay', 'firmware'))
        say(f'  overlay {supply.get("overlay", "firmware")}/ -> {copied} file(s) replaced')
        for item in materialize(work, board['board']['name']):
            say(f'  derive {item}')

        # One commit, dated with the upstream commit time: firmware.py reads exactly this for
        # SOURCE_DATE_EPOCH, and EDK II puts a timestamp in the volume.
        date = supply['commit_time']
        say(f'  git init, commit dated {date}')
        for command in (('init', '-q'), ('config', 'user.email', 'supply@localhost'),
                        ('config', 'user.name', 'supply-firmware'), ('add', '-A', '-f')):
            result = git('-C', work, *command)
            if result.returncode:
                raise SystemExit(f'git {command[0]} failed: {result.stderr.strip()}')
        env = dict(os.environ, GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date)
        result = git('-C', work, 'commit', '-qm', f'{supply["repo"]} @ {supply["commit"][:12]}', env=env)
        if result.returncode:
            raise SystemExit(f'git commit failed: {result.stderr.strip() or result.stdout.strip()}')

        # What the build will actually use, so a failure downstream points at the right input.
        epoch = git('-C', work, 'log', '-1', '--format=%ct').stdout.strip()
        say('')
        say(f'checkout {work}')
        say(f'  commit      {git("-C", work, "log", "-1", "--format=%H").stdout.strip()}')
        say(f'  SOURCE_DATE_EPOCH {epoch}')
        for probe in ('Resources/Scripts/mkbootimg.py', 'Platforms/Xiaomi/popsiclePkg/popsicle.dsc',
                      'Resources/Configs/popsicle.toml', 'BootShim/BootShim.S',
                      'Binaries/popsicle/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf',
                      f'Resources/DTBs/{board["board"]["name"]}.dtb',
                      'Resources/MuPatches/Gic-Spurious.patch'):
            say(f'  {"ok  " if (work / probe).is_file() else "MISS"} {probe}')
        missing = [p for p in ('Resources/Scripts/mkbootimg.py', 'Platforms/Xiaomi/popsiclePkg/popsicle.dsc',
                               'Resources/Configs/popsicle.toml', 'BootShim/BootShim.S',
                               f'Resources/DTBs/{board["board"]["name"]}.dtb',
                               'Resources/MuPatches/Gic-Spurious.patch')
                   if not (work / p).is_file()]
        if missing:
            raise SystemExit(f'{len(missing)} required file(s) missing; the overlay is incomplete')
        say('')
        say(f'pass this to the firmware step:  --uefi {work}')
        return 0
    except BaseException:
        # A half-assembled checkout that looks whole is worse than none: the build would fail later,
        # with an error about a driver rather than about the input.
        shutil.rmtree(work, ignore_errors=True)
        raise
    finally:
        shutil.rmtree(staging, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
