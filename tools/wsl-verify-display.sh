#!/bin/bash
cd /root/build/mu-silicium || exit 1
export UNIXNU_UEFI=/root/build/mu-silicium
echo "=== check.json display fields ==="
python3 -c "
import json
d=json.load(open('build/popsicle/check.json'))
for k in ('descriptors','source_reservations','display_reserved','splash_region','static_errors','static_warnings','missing_vendor_inputs'):
    print(f'  {k}: {d.get(k)}')
"
echo
echo "=== board.toml dtb_node / commented lines ==="
grep -n "dtb_node" /root/build/UniXNU-popsicle/board.toml || echo "(none)"
echo
echo "=== bootimg ==="
cd /root/build/unixnu || exit 1
python3 unixnu bootimg -D /root/build/UniXNU-popsicle --uefi /root/build/mu-silicium 2>&1 | tail -5
echo
echo "=== boot.json ==="
python3 -c "
import json
d=json.load(open('/root/build/unixnu/out/popsicle/boot/boot.json'))
for k,v in d.items(): print(f'  {k}: {v}')
"
