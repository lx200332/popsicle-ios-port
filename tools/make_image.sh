#!/bin/bash
# make_image.sh -- build a flashable loader boot image from a kernel Mach-O.
#
#   make_image.sh KERNEL OUT.img [--cmdline "..."] [--tc "TC0 TC1"] [--ref-adt REF.adt]
#
# --ref-adt carries device-pinned /chosen state (UFS extents, Wi-Fi MAC) over from a
# known-good build; without it the kernel boots but cannot mount its root filesystem.
# Build one with tools/extract_adt.py from any boot image that already works, e.g.
#   tools/extract_adt.py boots/loader-ct5-boot.img.gz /tmp/ct5.adt
#
# Replaces the lost build_loader2.sh + mkpayload-real.py pair:
#   kernel -> mkadt_pandora.py (ADT) -> mkpayload.py (SWEETXNU payload)
#          -> addtrustcache.py (trust caches) -> shim/loader build -> mkbootimg_v4.py
# Everything runs here; the only external tool is the WSL clang/lld in shim/loader.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
K=$1; OUT=$2; shift 2
CMD=""
TC=""
REF=""
ADTFILE=""
while [ $# -gt 0 ]; do
	case $1 in
		--cmdline) CMD=$2; shift 2 ;;
		--tc) TC=$2; shift 2 ;;
		--ref-adt) REF=$2; shift 2 ;;
		--adt-file) ADTFILE=$2; shift 2 ;;
		*) echo "unknown arg $1" >&2; exit 1 ;;
	esac
done
W=${WORK:-$(dirname "$OUT")/work}
rm -rf "$W"; mkdir -p "$W"

echo "=== 1 ADT ==="
# --gpu-pool carves the 1 GiB buffer pool off the top of the RAM handed to XNU, so
# XNU's dram-size and the pool never overlap (pandora_gpu.c allocates from the pool;
# it must be memory the kernel does not own). ct5 uses exactly this split:
# dram-size 0xfff00000 + pool <0x9bff00000,0x40000000> = 0x140000000 total.
python3 "$REPO/tools/mkadt_pandora.py" "$W/phone.adt" --gpu-pool "${GPU_POOL_MIB:-1024}" 2>&1 | tail -3
if [ -n "$ADTFILE" ]; then
	# A ready-made ADT (built from this device's own pinned-file extents); it already
	# carries /chosen extents, so no merge step is needed or wanted.
	cp "$ADTFILE" "$W/phone.adt"
	echo "adt: copied from $ADTFILE"
fi
if [ -n "$REF" ] && [ -z "$ADTFILE" ]; then
	# Device-pinned /chosen state an earlier pandora boot captured: UFS root/log/fw
	# block extents — without them ufsroot cannot mount — and the Wi-Fi MAC.
	# The GPU pool is NOT copied: it is recomputed above, since its base depends on
	# this build's dram-size.
	python3 "$REPO/tools/adt_merge_chosen.py" "$W/phone.adt" "$REF" \
		fuxi-root-extents fuxi-root-blocks pandora-log-extents pandora-log-blocks \
		pandora-fw-extents pandora-fw-blocks mac-address-wifi0 --force 2>&1
fi
ls -l "$W/phone.adt"

echo
echo "=== 2 payload ==="
# virt_base must sit below the LOWEST segment, not below __TEXT: the kernel's __LINKEDIT
# lives at a lower VA than the mach header, and the loader maps every segment relative
# to virt_base (wsl/make_phone_payload.sh does the same edit).
python3 - "$REPO" "$W" <<'PY'
import os, re, sys
repo, w = sys.argv[1], sys.argv[2]
src = open(os.path.join(repo, 'tools', 'mkpayload.py'), encoding='utf-8').read()
src = re.sub(r"virt_base = text\['vmaddr'\] & ~\(0x2000000 - 1\)",
             "virt_base = min(s['vmaddr'] for s in segs) & ~(0x2000000 - 1)", src)
open(os.path.join(w, 'mkpayload.py'), 'w', encoding='utf-8').write(src)
for f in ('adt.py',):
    open(os.path.join(w, f), 'w', encoding='utf-8').write(
        open(os.path.join(repo, 'tools', f), encoding='utf-8').read())
PY
if [ -n "$CMD" ]; then
	python3 "$W/mkpayload.py" "$K" "$W/phone.adt" "$W/payload.bin" --cmdline "$CMD" 2>&1 | tail -3
else
	python3 "$W/mkpayload.py" "$K" "$W/phone.adt" "$W/payload.bin" 2>&1 | tail -3
fi
cp "$W/payload.bin" "$W/payload-nopre.bin"

if [ -n "$TC" ]; then
	echo
	echo "=== 3 trust cache ==="
	# shellcheck disable=SC2086
	python3 "$REPO/tools/addtrustcache.py" "$W/payload-nopre.bin" "$W/payload-tc.bin" $TC
	mv "$W/payload-tc.bin" "$W/payload.bin"
fi

echo
echo "=== 4 loader ($(stat -c%s "$W/payload.bin") B payload) ==="
ls "$REPO/shim/loader/out" 2>/dev/null || true
mkdir -p "$REPO/shim/loader/out"
cp "$W/payload.bin" "$REPO/shim/loader/out/payload.bin"
[ -f "${LOGO:-}" ] && python3 -I "$REPO/tools/bootlogo.py" "$LOGO" "$REPO/shim/loader/out/logo.bin"
CC=${CC:-clang} LD=${LD:-ld.lld} bash "$REPO/shim/loader/build_linux.sh" 2>&1 | tail -6

echo
echo "=== 5 boot image ==="
cp "$REPO/shim/loader/out/loader-boot.img" "$OUT"
ls -l "$OUT"
sha256sum "$OUT"
