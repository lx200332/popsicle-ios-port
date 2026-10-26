#!/usr/bin/env python3
"""Derive the two values the popsicle device repo still guesses, from a live handset.

  gpt    <image>   userdata first_lba / last_lba out of a raw GPT dump
  iomem  <text>    dram base and extent out of /proc/iomem
  selftest         exercise both parsers on synthetic input

The GPT parser reads the on-disk structures directly rather than shelling out to sgdisk or parted:
neither is reliably present in an Android userland, while `dd` off the block device always works.
"""
import struct
import sys

SECTOR = 512
GPT_SIGNATURE = b'EFI PART'
GPT_ENTRY_SIZE = 128


def parse_gpt(data):
    """{name: (first_lba, last_lba)} for every non-empty partition in a GPT image.

    The header is located by signature rather than assumed to be at offset 0, so a dump taken from
    LBA 0 (protective MBR first) and one taken from LBA 1 (header first) both parse. Entry offsets are
    computed relative to whichever sector the header turned up in.
    """
    header_lba = None
    for candidate in range(0, min(9, len(data) // SECTOR)):
        if data[candidate * SECTOR: candidate * SECTOR + 8] == GPT_SIGNATURE:
            header_lba = candidate
            break
    if header_lba is None:
        raise SystemExit('no GPT header: no sector in the first 8 starts with "EFI PART"')

    header = data[header_lba * SECTOR:]
    entries_lba, count, size = struct.unpack_from('<QII', header, 72)
    if size < GPT_ENTRY_SIZE:
        raise SystemExit(f'implausible GPT entry size {size}')
    if not 1 <= count <= 4096:
        raise SystemExit(f'implausible GPT entry count {count}')

    # Two dump shapes have to work. A whole-disk dump has the protective MBR in sector 0, so the
    # signature lands on sector 1 and the entry array is at its absolute offset entries_lba*512.
    # A header-first dump starts at LBA 1, so the signature lands on sector 0 and every LBA is one
    # sector early.
    if header_lba >= 1:
        base = entries_lba * SECTOR
    else:
        base = (entries_lba - 1) * SECTOR
    if base < 0 or base >= len(data):
        raise SystemExit(f'partition entry array at offset {base} is outside the {len(data)}-byte dump')
    partitions = {}
    for index in range(count):
        entry = data[base + index * size: base + index * size + GPT_ENTRY_SIZE]
        if len(entry) < GPT_ENTRY_SIZE:
            break
        # An all-zero type GUID marks an unused slot.
        if not any(entry[:16]):
            continue
        first, last = struct.unpack_from('<QQ', entry, 32)
        name = entry[56:128].decode('utf-16-le', 'replace').split('\0')[0]
        partitions[name] = (first, last)
    return partitions


def parse_iomem(text):
    """The System RAM ranges from /proc/iomem, as [(base, size)] merged where adjacent."""
    ranges = []
    for line in text.splitlines():
        if 'System RAM' not in line:
            continue
        span = line.split(':', 1)[0].strip()
        if '-' not in span:
            continue
        low, high = span.split('-', 1)
        start, end = int(low, 16), int(high, 16) + 1
        ranges.append((start, end - start))

    ranges.sort()
    merged = []
    for start, size in ranges:
        if merged and start <= merged[-1][0] + merged[-1][1]:
            previous_start, previous_size = merged[-1]
            merged[-1] = (previous_start, max(previous_start + previous_size, start + size) - previous_start)
        else:
            merged.append((start, size))
    return merged


def build_gpt(names):
    """A synthetic GPT image: protective MBR at LBA 0, header at LBA 1, entries from LBA 2."""
    mbr = bytearray(SECTOR)
    # One 0xEE entry covering the disk, which is what a real protective MBR carries.
    mbr[446 + 4] = 0xEE
    struct.pack_into('<II', mbr, 446 + 8, 1, 0xFFFFFFFF)

    header = bytearray(SECTOR)
    header[0:8] = GPT_SIGNATURE
    struct.pack_into('<I', header, 8, 0x00010000)
    struct.pack_into('<I', header, 12, 92)
    entries_lba = 2
    count, size = 128, GPT_ENTRY_SIZE
    struct.pack_into('<Q', header, 24, 1)
    struct.pack_into('<Q', header, 32, 1000)
    struct.pack_into('<Q', header, 40, 34)
    struct.pack_into('<Q', header, 48, 900)
    struct.pack_into('<Q', header, 72, entries_lba)
    struct.pack_into('<I', header, 80, count)
    struct.pack_into('<I', header, 84, size)

    entries = bytearray(count * size)
    for index, (name, first, last) in enumerate(names):
        offset = index * size
        entries[offset:offset + 16] = bytes(range(1, 17))
        struct.pack_into('<QQ', entries, offset + 32, first, last)
        encoded = name.encode('utf-16-le')[:72]
        entries[offset + 56:offset + 56 + len(encoded)] = encoded
    return bytes(mbr) + bytes(header) + bytes(entries)


def selftest():
    image = build_gpt([('userdata', 0x2F3840, 0x3D0ABFA), ('metadata', 0x1000, 0x1FFF)])
    partitions = parse_gpt(image)
    assert partitions['userdata'] == (0x2F3840, 0x3D0ABFA), partitions
    assert partitions['metadata'] == (0x1000, 0x1FFF), partitions
    assert len(partitions) == 2, partitions

    text = """\
80000000-ffffffff : System RAM
  80080000-81ffffff : System RAM
  100000000-47fffffff : System RAM
  a0000000-a0000fff : not RAM
"""
    # The child ranges are kernel sub-allocations of the parent and the two top-level ranges are
    # adjacent, so all of it collapses to one 16 GiB window -- which is exactly the answer the ADT
    # wants when DRAM really is contiguous.
    merged = parse_iomem(text)
    assert merged == [(0x80000000, 0x400000000)], [hex(a) for a in merged[0]]

    # A gap must survive merging rather than being closed over.
    gapped = parse_iomem('80000000-8fffffff : System RAM\n90000000-9fffffff : System RAM\n')
    assert gapped == [(0x80000000, 0x20000000)], gapped
    split = parse_iomem('80000000-8fffffff : System RAM\n94000000-9fffffff : System RAM\n')
    assert split == [(0x80000000, 0x10000000), (0x94000000, 0xC000000)], split

    print('selftest: GPT and iomem parsers agree with hand-computed answers')


def main(argv):
    if argv == ['selftest']:
        selftest()
        return
    if len(argv) != 2:
        raise SystemExit(__doc__)
    command, argument = argv

    if command == 'gpt':
        partitions = parse_gpt(open(argument, 'rb').read())
        if 'userdata' not in partitions:
            raise SystemExit(f'no partition named "userdata"; saw {sorted(partitions)}')
        first, last = partitions['userdata']
        print(f'userdata  first_lba = {first:#x}  last_lba = {last:#x}  '
              f'({(last - first + 1) * SECTOR // (1 << 30)} GiB)')
        return

    if command == 'iomem':
        ranges = parse_iomem(open(argument, encoding='utf-8', errors='replace').read())
        if not ranges:
            raise SystemExit('no "System RAM" ranges found in that /proc/iomem dump')
        for start, size in ranges:
            print(f'System RAM  {start:#012x} + {size:#012x}  ({size / (1 << 30):.2f} GiB)')
        total = sum(size for _, size in ranges)
        print(f'total       {total:#012x}  ({total / (1 << 30):.2f} GiB)')
        print()
        print('For [payload] dram_size the ADT takes one contiguous window. Use the low range that')
        print('contains the kernel load address (0xC7800000) unless the whole span is contiguous.')
        return

    raise SystemExit(f'unknown command {command!r}')


if __name__ == '__main__':
    main(sys.argv[1:])
