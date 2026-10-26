#!/bin/bash
cd <BUILD>/mu-silicium || exit 1
echo "===== build_uefi.py (first 120 lines) ====="
head -120 build_uefi.py
echo
echo "===== grep for infiniti in build_uefi.py ====="
grep -n "infiniti" build_uefi.py | head
