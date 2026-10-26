#!/usr/bin/env python3
"""Prove the UniXNU loader is inside the boot image we are about to flash.

The chain is long enough that each stage can look correct while the next one silently drops its
input, so this walks the image the way the bootloader will, and fails loudly at the first step that
does not add up:

    Android boot header v0 -> gzip(kernel) -> BootShim + firmware volume -> LZMA -> FVMAIN -> FFS files

The firmware volume is compressed with LZMA (LzmaCustomDecompressGuid), not gzip, and the device tree
is appended to the kernel field as raw bytes after the gzip member -- so decompressing the kernel with
gzip alone raises BadGzipFile on the first DTB byte. zlib.decompressobj(31) stops at the member end
and leaves the tree in unused_data, which is what this does.
"""
import lzma
import struct
import sys
import zlib
from pathlib import Path

GZIP_WBITS = 31
SHIM_BYTES = 192
FD_BYTES = 0x400000
DTB_MAGIC = b'\xd0\x0d\xfe\xed'
LZMA_GUID = bytes.fromhex('98584eee143959429d6edc7bd79403cf')
# EFI_FIRMWARE_FILE_SYSTEM2_GUID = 8c8ce578-8a3d-4f1c-9935-896185c32dd3, in FFS byte order.
FFS2_GUID = bytes.fromhex('78e58c8c3d8a1c4f9935896185c32dd3')

# The three component FFS directory GUIDs from Guid.xref, encoded the way FFS stores them:
# Data1 (32 bit), Data2 and Data3 (16 bit each) little endian, Data4 (8 bytes) verbatim.
#   UniXNU      9B665204-A133-47A2-9396-9E2B370A4E25
#   UniXNUDxe   29B950CF-586D-49EA-9D71-F88645CCDAF6
#   DarwinBoot  1B64AF87-8415-4B57-9B16-680D70913B9F
FFS_UNIXNU = bytes.fromhex('0452669b33a1a24793969e2b370a4e25')
FFS_UNIXNUDXE = bytes.fromhex('cf50b9296d58ea499d71f88645ccdaf6')
FFS_DARWINBOOT = bytes.fromhex('87af641b1584574b9b16680d70913b9f')


def fail(message):
    print(f'  FAIL {message}')
    sys.exit(1)


def main(path):
    data = Path(path).read_bytes()
    print(f'image {len(data)} bytes')

    # ---- Android boot header v0 -------------------------------------------------------------
    magic, kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr, \
        tags_addr, page_size, header_version = struct.unpack_from('<8s9I', data, 0)
    if magic != b'ANDROID!':
        fail(f'magic is {magic!r}, not ANDROID!')
    print(f'  header v{header_version} page {page_size} kernel {kernel_size} at {kernel_addr:#x} '
          f'tags {tags_addr:#x}')
    if header_version != 0:
        fail(f'header_version {header_version}, expected 0 for UniXNU')
    if page_size != 4096:
        fail(f'page_size {page_size}, expected 4096')
    if kernel_addr != 0x8000:
        fail(f'kernel_address {kernel_addr:#x}, expected 0x8000 (base 0 + kernel_offset 0x8000)')
    if tags_addr != 0x1e00000:
        fail(f'tags_address {tags_addr:#x}, expected 0x1e00000')
    if second_size != 0:
        fail(f'second_size {second_size}, expected 0')

    # ---- kernel field: gzip(shim + firmware volume) + raw device tree ------------------------
    kernel = data[page_size:page_size + kernel_size]
    inflate = zlib.decompressobj(GZIP_WBITS)
    payload = inflate.decompress(kernel) + inflate.flush()
    tree = inflate.unused_data
    print(f'  gzip -> {len(payload)} bytes payload, {len(tree)} bytes appended tree')
    if len(payload) != SHIM_BYTES + FD_BYTES:
        fail(f'payload is {len(payload)}, expected {SHIM_BYTES + FD_BYTES} (shim + {FD_BYTES:#x})')
    if not tree.startswith(DTB_MAGIC):
        fail(f'appended data starts {tree[:4]!r}, not an FDT magic')
    total = struct.unpack_from('>I', tree, 4)[0]
    print(f'  device tree {total} bytes, magic ok')

    shim, firmware = payload[:SHIM_BYTES], payload[SHIM_BYTES:]
    if shim[56:60] != b'ARMd':
        fail(f'shim ARM64 magic is {shim[56:60]!r}')
    text_offset, image_size = struct.unpack_from('<QQ', shim, 8)
    print(f'  shim {SHIM_BYTES} bytes, text_offset {text_offset:#x}, image_size {image_size:#x}')
    if image_size != SHIM_BYTES + FD_BYTES:
        fail(f'shim image_size {image_size:#x} does not describe shim + volume')

    # ---- firmware volume: FFS2, then the LZMA-compressed FVMAIN -----------------------------
    # The volume opens with the FFS header of the first file (00040014...), not a zero vector; the
    # filesystem GUID follows at 16 as 8c8ce578-8a3d-4f1c-9935-896185c32dd3 in FFS byte order.
    fs_guid = firmware[16:32]
    if fs_guid != FFS2_GUID:
        fail(f'filesystem GUID is {fs_guid.hex()}, expected {FFS2_GUID.hex()}')
    fv_length = struct.unpack_from('<Q', firmware, 32)[0]
    print(f'  outer volume {fv_length:#x} bytes, FFS2')
    if fv_length != FD_BYTES:
        fail(f'volume length {fv_length:#x}, expected {FD_BYTES:#x}')

    # The guided section header is GUID(16) + DataOffset(2) + Attributes(2) = 20 bytes, and the
    # LZMA-alone stream starts immediately after it: 5 property bytes (0x5D, dict 16 MiB) then the
    # 8-byte uncompressed size. DataOffset points at the compressed payload, i.e. four bytes into
    # that stream, so it is not the stream start.
    at = firmware.find(LZMA_GUID)
    if at < 0:
        fail('no LzmaCustomDecompressGuid section in the volume')
    start = at + 20
    print(f'  LZMA section at {at:#x}, stream starts {start:#x}')
    if firmware[start] != 0x5D:
        fail(f'LZMA properties byte is {firmware[start]:#x}, expected 0x5D')

    inner = lzma.decompress(firmware[start:], format=lzma.FORMAT_ALONE)
    print(f'  decompressed FVMAIN {len(inner)} bytes')
    fvh = inner.find(b'_FVH')
    if fvh < 0:
        fail('inner volume has no _FVH signature')
    print(f'  _FVH at {fvh:#x}')

    # ---- the three components -----------------------------------------------------------------
    for name, guid, must_pack in (
            ('UniXNU', FFS_UNIXNU, True),
            ('UniXNUDxe', FFS_UNIXNUDXE, True),
            ('DarwinBoot', FFS_DARWINBOOT, False)):
        found = inner.find(guid)
        if must_pack and found < 0:
            fail(f'{name} is not in the packed volume')
        if not must_pack and found >= 0:
            fail(f'{name} is packed, but it should stay unpacked')
        print(f'  {name:<10} {"present at " + hex(found) if found >= 0 else "absent (as intended)"}')

    print('  OK the image carries the UniXNU loader, its DXE driver and the board device tree')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'boot-popsicle-unixnu.img')
