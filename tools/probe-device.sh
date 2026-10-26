#!/bin/bash
# Collect the two values the popsicle device repo still guesses, from a connected handset.
#
#   tools/probe-device.sh            # probe and print a ready-to-paste TOML snippet
#   tools/probe-device.sh --write    # also patch board.toml in the port workspace
#
# Neither sgdisk nor parted is assumed: the GPT is read with dd straight off the block device and
# parsed locally, and RAM comes from /proc/iomem. Root is needed for both, which the handset has.
set -u
ADB=${ADB:-/c/dsh-tools/cli/adb/adb.exe}
PY=${PY:-/c/dsh-tools/cli/python315/python.exe}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$(mktemp -d)}
BOARD=${BOARD:-$HERE/../UniXNU-popsicle/board.toml}

WRITE=0
[ "${1:-}" = "--write" ] && WRITE=1

say() { printf '%s\n' "$*"; }

if ! "$ADB" get-state >/dev/null 2>&1; then
  say "no device: adb reports no attached handset"
  say "connect it, enable USB debugging, and confirm 'adb devices' lists it"
  exit 2
fi

say "device: $("$ADB" shell getprop ro.product.device | tr -d '\r')  ($("$ADB" shell getprop ro.serialno | tr -d '\r'))"

# --- userdata GPT -----------------------------------------------------------------------------
say
say "reading the primary GPT off LBA 0 (needs root)..."
"$ADB" shell 'su -c "dd if=/dev/block/sda bs=512 count=34 2>/dev/null"' > "$WORK/gpt.bin"
say "  got $(wc -c < "$WORK/gpt.bin") bytes"

GPT_OUT=$("$PY" "$HERE/probe-device.py" gpt "$WORK/gpt.bin" 2>&1)
GPT_STATUS=$?
say "  $GPT_OUT"

# --- physical RAM -----------------------------------------------------------------------------
say
say "reading /proc/iomem..."
"$ADB" shell 'su -c "cat /proc/iomem"' > "$WORK/iomem.txt" 2>/dev/null
say "  got $(wc -l < "$WORK/iomem.txt") lines"

IOMEM_OUT=$("$PY" "$HERE/probe-device.py" iomem "$WORK/iomem.txt" 2>&1)
IOMEM_STATUS=$?
printf '%s\n' "$IOMEM_OUT" | sed 's/^/  /'

# --- emit -------------------------------------------------------------------------------------
say
if [ $GPT_STATUS -ne 0 ]; then
  say "GPT parse failed; [userdata] stays unset."
else
  FIRST=$(printf '%s' "$GPT_OUT" | sed -n 's/.*first_lba = \(0x[0-9a-f]*\).*/\1/p')
  LAST=$(printf '%s' "$GPT_OUT" | sed -n 's/.*last_lba = \(0x[0-9a-f]*\).*/\1/p')
  say "TOML for [userdata]:"
  say "  first_lba = $FIRST"
  say "  last_lba  = $LAST"
  say "  lba_bytes = 4096"
fi

say
if [ $IOMEM_STATUS -ne 0 ]; then
  say "RAM parse failed; [payload] dram_size stays at its conservative default."
else
  # The lowest contiguous System RAM window that spans the kernel load address is the one the ADT
  # should describe; 0xC7800000 is PcdSystemMemoryBase + the kernel offset from the vendor map.
  say "TOML for [payload]:"
  say "  dram_size = <see the window above that contains 0xC7800000>"
fi

if [ $WRITE -eq 1 ]; then
  say
  say "patching $BOARD is not implemented yet: confirm the numbers above by hand first,"
  say "then add the [userdata] section and adjust [payload] dram_size."
fi

say
say "scratch files kept in $WORK"
