"""Let ipsw.py decode the kernelcache with lzfse when compression_tool is absent.

`ipsw.kernelcache()` is the only place in the whole chain that requires macOS:

    ipsw.py:205   tool('compression_tool', 'macOS /usr/bin/compression_tool decodes the LZFSE kernelcache')
    ipsw.py:210   subprocess.run(['/usr/bin/compression_tool', '-decode', '-a', 'lzfse', '-i', packed, '-o', raw])

Everything around it is portable — the zip-over-HTTP-range reader, the IM4P envelope,
the fat-slice pick, the pin check. The compressed stream is LZFSE, and the open-source
`lzfse` package decodes the identical format (`lzfse -decode -i IN -o OUT`), so the
dependency is on one binary, not on the platform.

This rewrites that step in the read-only UniXNU checkout to fall back to `lzfse` when
`/usr/bin/compression_tool` is missing. It is deliberately a patch applied at build
time rather than a fork: the checkout is a pinned, disposable artefact, and keeping
the change here means `unixnu.lock` still names exactly the upstream revision.

The substitution is only accepted if the result hashes to the board pin, which
`ipsw.py:229` already enforces — so a decoder that produces different bytes fails
loudly instead of yielding a silently wrong kernelcache.

Usage: patch-unixnu-kernelcache.py UNIXNU_CHECKOUT [--revert]
"""
import sys
from pathlib import Path

MARKER = '# popsicle: lzfse fallback'

ORIGINAL_TOOL = """    tool('compression_tool', 'macOS /usr/bin/compression_tool decodes the LZFSE kernelcache')"""

PATCHED_TOOL = """    # popsicle: lzfse fallback
    # compression_tool is macOS-only; the stream it reads is LZFSE, which the
    # open-source lzfse package decodes byte-for-byte identically. The pin check at
    # the end of this function is what makes the substitution safe.
    decoder = None
    if Path('/usr/bin/compression_tool').is_file():
        decoder = ['/usr/bin/compression_tool', '-decode', '-a', 'lzfse']
    elif shutil.which('lzfse'):
        decoder = ['lzfse', '-decode']
    else:
        tool('compression_tool or lzfse',
             'decodes the LZFSE kernelcache (macOS /usr/bin/compression_tool, or apt install lzfse)')"""

ORIGINAL_RUN = """    result = subprocess.run(['/usr/bin/compression_tool', '-decode', '-a', 'lzfse', '-i', packed, '-o', raw], capture_output=True, text=True)"""

PATCHED_RUN = """    result = subprocess.run(decoder + ['-i', packed, '-o', raw], capture_output=True, text=True)"""

ORIGINAL_ERROR = """        raise Error(f'compression_tool failed: {result.stderr.strip()}')"""

PATCHED_ERROR = """        raise Error(f'{decoder[0]} failed: {result.stderr.strip()}')"""

REQUIRED_IMPORT = 'import shutil'


def patch(path, revert=False):
    text = path.read_text()
    if revert:
        if MARKER not in text:
            return 'was not patched'
        text = text.replace(PATCHED_TOOL, ORIGINAL_TOOL)
        text = text.replace(PATCHED_RUN, ORIGINAL_RUN)
        text = text.replace(PATCHED_ERROR, ORIGINAL_ERROR)
        # The import went in for the patch alone, so take it back out; otherwise a
        # revert leaves a one-line diff and the tree is no longer provably upstream.
        text = text.replace(f'import subprocess\n{REQUIRED_IMPORT}\n', 'import subprocess\n', 1)
        path.write_text(text)
        return 'reverted'
    if MARKER in text:
        return 'already patched'
    for original, patched in ((ORIGINAL_TOOL, PATCHED_TOOL),
                              (ORIGINAL_RUN, PATCHED_RUN),
                              (ORIGINAL_ERROR, PATCHED_ERROR)):
        if original not in text:
            raise SystemExit(f'{path}: cannot find the text to replace:\n{original}')
        text = text.replace(original, patched, 1)
    if REQUIRED_IMPORT not in text:
        # Put the import with the others rather than at the top of the file.
        anchor = 'import subprocess'
        if anchor not in text:
            raise SystemExit(f'{path}: no "import subprocess" to anchor the shutil import')
        text = text.replace(anchor, f'{anchor}\n{REQUIRED_IMPORT}', 1)
    path.write_text(text)
    return 'patched'


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    checkout = Path(argv[0])
    revert = '--revert' in argv
    target = checkout / 'scripts' / 'unixnu' / 'ipsw.py'
    if not target.is_file():
        raise SystemExit(f'{target} does not exist: is {checkout} a UniXNU checkout?')
    if 'def kernelcache(' not in target.read_text():
        raise SystemExit(f'{target} has no kernelcache(): the pinned revision moved')
    print(f'{target}: {patch(target, revert)}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
