#!/bin/bash
# Patch build_uefi.py so [boot_image] values (including base = 0) reach mkbootimg, then rebuild.
set -e
python3 /mnt/c/dsh-对话/popsicle-port/tools/patch-builduefi.py
cd /root/build/mu-silicium
grep -n 'pagesize\|is not None' build_uefi.py | head
echo "=== rebuild ==="
python3 build_uefi.py -d popsicle 2>&1 | tail -6
echo "=== header ==="
python3 -c "
import struct
d=open('/root/build/mu-silicium/Mu-popsicle.img','rb').read()
f=struct.unpack_from('<8s10I16s',d,0)
names=['kernel_size','kernel_addr','ramdisk_size','ramdisk_addr','second_size','second_addr','tags_addr','page_size','header_version','os_version']
print('total', len(d), 'magic', f[0])
for n,v in zip(names,f[1:11]):
    print(f'{n:16s} {v:#010x}  ({v})')
"
