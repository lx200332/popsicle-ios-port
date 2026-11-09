#!/bin/bash
# Register the PANDORA / Popsicle drivers in bsd/conf/files, and wire their
# init calls into the sweet_ufs.c bring-up block.
#
# The patch xnu/patches/0335-pandora-conf-files.patch is the record of what this
# script does; it is not the mechanism.  A static patch cannot ask "is this line
# already there", so applying it twice fails:
#
#   error: patch failed: bsd/conf/files:134
#
# This script is the idempotent version.  Run it as often as you like:
#
#   tools/register-drivers.sh [/path/to/xnu]
#
# The default /opt/work/ci/buildtree/xnu is the Ubuntu-24.04 build tree, so this
# normally runs under WSL.  Nothing outside that tree is written.
#
# NOTE: on Windows the bare `python3` in Git-Bash is the App Installer stub (it
# prints nothing and exits 49), so run this from WSL.
set -euo pipefail

XNU=${1:-/opt/work/ci/buildtree/xnu}
CF=$XNU/bsd/conf/files
DEV=$XNU/bsd/dev
UFS=$DEV/sweet_ufs.c

[ -f "$CF" ] || { echo "no bsd/conf/files under $XNU" >&2; exit 1; }

python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 6) else 1)' || {
  echo "python3 is not usable here (on Windows use WSL: the App Installer stub exits 49)" >&2
  exit 1
}

# conf/files carries a source file per line; header files are found through the
# include path and are never listed here (the file has no .h entry at all).
#
# The first eleven are the probe / thermal / touch set.  The last three come
# from the nine-module adaptation round: sensor instrumentation, the cpuidle
# census and the VM observability driver.  All fourteen are boot-arg gated inside
# the driver, so registering them cannot change behaviour on a boot that does
# not pass the matching argument.
drivers="pandora_bt pandora_modem pandora_thermal popsicle_wifi popsicle_audio
popsicle_camera popsicle_fp popsicle_modem popsicle_syna popsicle_goodix
redwood_input pandora_sensors pandora_idle pandora_mem"

# pandora_touch.c sits above bsd/dev/pandora_amp.c in the upstream tree, outside
# the block this script appends to.  It is listed separately so the anchor walk
# below never lands above it: this script verifies it is present, and says so
# loudly if a tree lost the line, but it does not move it.
preregistered="pandora_touch"

python3 - "$CF" "$DEV" "$UFS" $preregistered -- $drivers <<'PY'
import os
import re
import sys

cf, dev, ufs = sys.argv[1], sys.argv[2], sys.argv[3]
rest = sys.argv[4:]
split = rest.index('--')
pre, drivers = rest[:split], rest[split + 1:]
rc = 0

# ------------------------------------------------------------- pre-registered --
# These already sit in the upstream file, outside the block appended below.  A
# missing line means the tree is not the one this script was written against, so
# say it once and keep going -- nothing here moves them.
for n in pre:
    if not os.path.isfile(os.path.join(dev, n + '.c')):
        print('MISSING   %s/%s.c (source not in the tree)' % (dev, n))
        rc = 1
    pat = re.compile(r'^bsd/dev/%s\.c(\s|$)' % re.escape(n))
    if not any(pat.match(l) for l in open(cf, encoding='utf-8').read().split('\n')):
        print('ABSENT    bsd/dev/%s.c is not in %s (upstream line lost?)' % (n, cf))
        rc = 1
    else:
        print('present   bsd/dev/%s.c (upstream position kept)' % n)

# ---------------------------------------------------------------- conf/files --
# New entries go after bsd/dev/pandora_dpu.c, the last PANDORA line in the file.
# The anchor walks forward as entries are added so the block keeps the order
# above no matter how many of them were already present.
lines = open(cf, encoding='utf-8').read().split('\n')
anchor = 'bsd/dev/pandora_dpu.c'
for n in drivers:
    if not os.path.isfile(os.path.join(dev, n + '.c')):
        print('MISSING   %s/%s.c' % (dev, n))
        rc = 1
        continue
    pat = re.compile(r'^bsd/dev/%s\.c(\s|$)' % re.escape(n))
    if any(pat.match(l) for l in lines):
        print('present   bsd/dev/%s.c' % n)
    else:
        idx = next((i for i, l in enumerate(lines)
                    if re.match(r'^%s(\s|$)' % re.escape(anchor), l)), None)
        if idx is None:
            sys.exit('no anchor %s in %s' % (anchor, cf))
        lines.insert(idx + 1, 'bsd/dev/%s.c\t\tstandard' % n)
        print('added     bsd/dev/%s.c' % n)
    anchor = 'bsd/dev/%s.c' % n
open(cf, 'w', encoding='utf-8').write('\n'.join(lines))

# -------------------------------------------------------------- sweet_ufs.c --
# Every driver exports <name>_init().  sweet_ufs.c declares them itself instead
# of including their headers, so the prototype has to go in as well or the call
# becomes an implicit declaration, which is an error under this build's flags.
#
# Prototype and call both have to stay inside a `#if defined(PANDORA)` block:
# outside it the non-PANDORA build would reference a symbol that is not compiled
# in.  The insertion point is therefore the last `<x>_init` line that sits
# inside such a block, whatever it happens to be -- no hard-coded anchor name,
# so the script works on a pristine tree and on an already-wired one.
EXTERN_RE = re.compile(r'^extern void (\w+)_init\(void\);')
CALL_RE = re.compile(r'^\s*(\w+)_init\(\);')


def pandora_depth(ls):
    """Per-line nesting depth of `#if defined(PANDORA)` blocks."""
    depth, out = 0, []
    for l in ls:
        if re.match(r'^#\s*if', l):
            if 'defined(PANDORA)' in l:
                depth += 1
                out.append(depth)          # the #if itself counts as inside
                continue
            out.append(depth)
            depth += 0 if False else 0     # other #ifs do not change PANDORA depth
            continue
        if re.match(r'^#\s*endif', l):
            out.append(depth)
            depth = max(0, depth - 1)
            continue
        out.append(depth)
    return out


def insert_after_last(text, name, needle, regex, make_line):
    if needle in text:
        return text, 'present'
    ls = text.split('\n')
    depth = pandora_depth(ls)
    idx = next((i for i in range(len(ls) - 1, -1, -1)
                if depth[i] > 0 and regex.match(ls[i])), None)
    if idx is None:
        sys.exit('no %s anchor inside a PANDORA block in sweet_ufs.c' % name)
    ls.insert(idx + 1, make_line())
    return '\n'.join(ls), 'added'


if not os.path.isfile(ufs):
    print('MISSING   %s' % ufs)
    rc = 1
else:
    text = open(ufs, encoding='utf-8').read()
    wiring = [
        ('redwood_input',   '/dev/redwood-touch queue (bsd/dev/redwood_input.c)'),
        ('pandora_sensors', 'sensor survey, boot-arg pandora_sensors (bsd/dev/pandora_sensors.c)'),
        ('pandora_idle',    'boot-arg gated, see bsd/dev/pandora_idle.c'),
        ('pandora_mem',     'boot-arg gated, see bsd/dev/pandora_mem.c'),
    ]
    for name, note in wiring:
        text, how = insert_after_last(
            text, name,
            'extern void %s_init(void);' % name, EXTERN_RE,
            lambda n=name: 'extern void %s_init(void);' % n)
        print('%-9s extern %s_init() in sweet_ufs.c' % (how, name))

        text, how = insert_after_last(
            text, name,
            '\t%s_init();' % name, CALL_RE,
            lambda n=name, t=note: '\t%s_init();\t/* %s */' % (n, t))
        print('%-9s %s_init() call in sweet_ufs.c' % (how, name))
    open(ufs, 'w', encoding='utf-8').write(text)

# ------------------------------------------------------------------- report --
print('---- conf/files entries ----')
body = open(cf, encoding='utf-8').read()
for n in drivers:
    print('%-18s %s' % (n, len(re.findall(r'^bsd/dev/%s\.c(\s|$)' % re.escape(n),
                                         body, re.M))))
sys.exit(rc)
PY
