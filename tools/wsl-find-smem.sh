#!/bin/bash
# Look for an SMEM RAM partition table in the extracted firmware, so dram_size stops being a guess.
E=/mnt/c/dsh-对话/popsicle-port/extract
echo "=== extract/ top level ==="
ls -la "$E" 2>/dev/null | head -40
echo
echo "=== candidate smem / devinfo / ddr images ==="
find "$E" -maxdepth 3 -type f \( -iname "*smem*" -o -iname "*devinfo*" -o -iname "*ddr*" -o -iname "*xbl*" -o -iname "*uefiplat*" \) 2>/dev/null | head -30
echo
echo "=== any uefiplat.cfg (stock MemoryMap) ==="
find "$E" -iname "*uefiplat*" 2>/dev/null | head
echo
echo "=== xbl_config memorymap node dir ==="
ls -la "$E/memmap" 2>/dev/null | head -20 || echo "(no memmap dir)"
