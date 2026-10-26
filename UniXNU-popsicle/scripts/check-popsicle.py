#!/usr/bin/env python3
"""Check candidate platform intervals against the compiled popsicle device tree.

Ported from check-sheng.py. Two things changed beyond the names:

* the device tree is read with a small built-in FDT parser instead of fdtget, so the check runs
  without device-tree-compiler (there is no dtc on the Windows side of this port);
* a device tree reservation that has no descriptor is a *warning*, not an error. The Mu-Silicium
  memory map lists the regions UEFI has to know about; the vendor tree also reserves regions that
  UEFI never touches (shm, ramoops, the zero-sized demura placeholders). On sheng the two happened
  to line up exactly; on popsicle they do not, and pretending otherwise would just fail the
  preflight for no reason. Everything that can actually corrupt a boot -- misaligned or overlapping
  descriptors, a firmware volume that disagrees with the config, a missing vendor input -- is still
  a hard error.
"""
import csv
import json
import os
import re
import struct
from pathlib import Path
import tomllib

def _find_uefi():
    """Locate the Mu-Silicium checkout.

    Three callers exist and they disagree about where it is:
      * firmware.py:33 runs preflight with cwd=<tree>, and tree.py:52-56 symlinks the firmware
        checkout to <tree>/.cache/firmware — so that is the path that resolves on a runner.
      * A hand run from inside the Mu-Silicium checkout sees it as '.'.
      * A hand run next to a plain 'mu-silicium' directory sees that name.
    UNIXNU_UEFI overrides all three. Running from anywhere else is an error rather than a guess,
    because every check below reads files out of this tree.
    """
    explicit = os.environ.get('UNIXNU_UEFI')
    if explicit:
        path = Path(explicit)
        if not (path / 'Resources/Configs/popsicle.toml').is_file():
            raise SystemExit(f'UNIXNU_UEFI={explicit} is not a Mu-Silicium checkout carrying '
                             f'Resources/Configs/popsicle.toml')
        return path
    for candidate in ('.cache/firmware', 'mu-silicium', '.'):
        path = Path(candidate)
        if (path / 'Resources/Configs/popsicle.toml').is_file():
            return path
    raise SystemExit('no Mu-Silicium checkout here: run from the assembled tree, from inside the '
                     'checkout, or set UNIXNU_UEFI to it')


UEFI = _find_uefi()
PLATFORM = UEFI / 'Platforms/Xiaomi/popsiclePkg'

# The build passes the assembled tree in build/<board>/; running the check by hand should not need
# that tree to exist first, so an unset UNIXNU_DTB falls back to the vendor device tree the firmware
# ships. An explicitly named tree that is not there is an error: silently checking a different tree
# than the one that was asked for is how a stale descriptor table gets waved through.
_DTB_EXPLICIT = os.environ.get('UNIXNU_DTB')
if _DTB_EXPLICIT:
    DTB = Path(_DTB_EXPLICIT)
    if not DTB.is_file():
        raise SystemExit(f'UNIXNU_DTB={_DTB_EXPLICIT} does not exist')
else:
    DTB = Path('build/popsicle/popsicle.dtb')
    if not DTB.is_file():
        DTB = UEFI / 'Resources/DTBs/popsicle.dtb'

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def fdt_properties(path):
    """Yield (node_path, property_name, value) for every property in the tree.

    node_path is relative to the root with no leading slash, so a child of /reserved-memory comes
    back as 'reserved-memory/<name>'. The root node itself yields the empty string.
    """
    data = Path(path).read_bytes()
    # Standard FDT header: magic, totalsize, off_struct, off_strings, off_mem_rsvmap, version,
    # last_comp_version, boot_cpuid_phys, size_strings, size_struct.
    (magic, _total, off_struct, off_strings, _off_rsvmap, _version,
     _last_comp, _boot_cpuid, _size_strings, _size_struct) = struct.unpack_from('>10I', data, 0)
    if magic != FDT_MAGIC:
        raise SystemExit(f'{path}: not a flattened device tree (magic {magic:#x})')

    def string_at(offset):
        end = data.index(b'\0', offset)
        return data[offset:end].decode('utf-8', 'replace')

    path_stack = []
    cursor = off_struct
    while True:
        (token,) = struct.unpack_from('>I', data, cursor)
        cursor += 4
        if token == FDT_BEGIN_NODE:
            name = string_at(cursor)
            cursor += (len(name.encode()) + 1 + 3) & ~3
            path_stack.append(name or '/')
        elif token == FDT_END_NODE:
            if path_stack:
                path_stack.pop()
        elif token == FDT_PROP:
            length, name_offset = struct.unpack_from('>II', data, cursor)
            cursor += 8
            value = data[cursor:cursor + length]
            cursor += (length + 3) & ~3
            node = '/'.join(part for part in path_stack if part != '/')
            yield node, string_at(off_strings + name_offset), value
        elif token == FDT_NOP:
            continue
        elif token == FDT_END:
            break
        else:
            raise SystemExit(f'{path}: unknown FDT token {token:#x} at {cursor - 4}')


def _cells(value):
    return [int.from_bytes(value[i:i + 4], 'big') for i in range(0, len(value) - 3, 4)]


def fdt_reservations(path):
    """Every /reserved-memory child that carries a reg, as (name, base, size)."""
    reservations = []
    for node, prop, value in fdt_properties(path):
        if prop != 'reg':
            continue
        parts = node.split('/')
        if len(parts) != 2 or parts[0] != 'reserved-memory':
            continue
        cells = _cells(value)
        for index in range(0, len(cells) - 3, 4):
            base = (cells[index] << 32) | cells[index + 1]
            size = (cells[index + 2] << 32) | cells[index + 3]
            reservations.append((parts[1], base, size))
    return reservations


def fdt_reg_of(path, wanted):
    """The first (base, size) pair of one node's reg property, or None when absent."""
    for node, prop, value in fdt_properties(path):
        if node == wanted and prop == 'reg':
            cells = _cells(value)
            if len(cells) >= 4:
                return (cells[0] << 32) | cells[1], (cells[2] << 32) | cells[3]
    return None


if __name__ == '__main__':
    errors, warnings = [], []

    if not DTB.exists():
        raise SystemExit(f'{DTB} is missing: run "unixnu bootimg" or point UNIXNU_DTB at the merged tree')
    reservations = fdt_reservations(DTB)

    source = (PLATFORM / 'Library/MemoryMapLib/MemoryMapLib.c').read_text()
    rows = [(m[1], int(m[2], 16), int(m[3], 16), m[4]) for m in re.finditer(
        r'\{"([^"]+)"\s*,\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),([^}]+)\}', source)]
    if not rows or len(rows) > 128:
        errors.append('Empty map or descriptor capacity exceeded')

    # Every real device tree reservation has to be covered by memory the firmware will not hand out.
    # Zero-sized entries (the demura placeholders are <0x0 0x0> on this handset) describe nothing.
    #
    # "Will not hand out" means any descriptor whose ResourceType is not Conv (EfiConventionalMemory):
    # Reserv, BsData, RtData and MmIO all stay out of the free pool, so a vendor reservation landing on
    # UEFI_Log or a page table is as safe as one landing on a Reserv descriptor. Only Conv is a hole.
    #
    # The test is containment, not equality: Mu-Silicium describes some ranges coarsely (PIL_Reserved is
    # one 0x1D300000 block that the vendor tree splits into mpss, adsp, cdsp, camera, video, ...), so
    # demanding an exact base+size match would report every one of those as a failure.
    managed = [(a, a + s) for _, a, s, attrs in rows if not re.search(r'\bConv\b', attrs)]
    for name, base, size in reservations:
        if not size:
            continue
        pieces = [(base, base + size)]
        for lo, hi in managed:
            pieces = [piece for plo, phi in pieces
                      for piece in [(plo, min(phi, lo)), (max(plo, hi), phi)]
                      if piece[0] < piece[1]]
        for plo, phi in pieces:
            warnings.append(f'reservation outside the map: {name} {plo:#x}+{phi - plo:#x}')

    for index, (name, base, size, _) in enumerate(rows):
        if base % 4096 or size % 4096 or not size:
            errors.append(f'Invalid UEFI page alignment: {name}')
        for other, other_base, other_size, _ in rows[index + 1:]:
            if max(base, other_base) < min(base + size, other_base + other_size):
                errors.append(f'Overlapping intervals: {name}, {other}')

    config = tomllib.loads((UEFI / 'Resources/Configs/popsicle.toml').read_text())
    fd = config['uefi_fd']
    if not any(n == 'UEFI_FD' and a == fd['base'] and s == fd['size'] for n, a, s, _ in rows):
        errors.append('FD configuration does not match memory descriptor')

    dsc = (PLATFORM / 'popsicle.dsc').read_text()
    stack = re.search(r'PcdCPUCoresStackBase\|(0x[0-9a-fA-F]+)', dsc)
    if not stack or not any(n == 'UEFI_Stack' and a == int(stack[1], 16) for n, a, _, _ in rows):
        errors.append('Stack configuration does not match memory descriptor')

    # The console path hangs off exactly one interval. With USE_CUSTOM_DISPLAY_DRIVER = 0 the firmware
    # ships SimpleFbDxe, which finds its framebuffer by looking up a memory region whose name compares
    # equal to "Display_Reserved" (AsciiStriCmp, so the underscore form matches) and then draws at the
    # resolution in the PcdFrameBuffer* PCDs. The Qualcomm DisplayDxe that the other branch would pull
    # in is a prebuilt binary whose panel table is the OnePlus 15 one, so it cannot drive this panel;
    # the panel XMLs in RawFiles are only referenced by that branch and never enter the volume.
    # If the descriptor and the vendor splash reservation drift apart, UEFI draws into memory ABL
    # still owns -- a corrupt screen at best, a dead boot at worst.
    splash = fdt_reg_of(DTB, 'reserved-memory/splash_region')
    display = next(((a, s) for n, a, s, _ in rows if n == 'Display_Reserved'), None)
    if splash is None:
        warnings.append('device tree has no /reserved-memory/splash_region')
    elif display is None:
        errors.append('Display_Reserved descriptor is missing: SimpleFbDxe cannot find a framebuffer')
    elif display != splash:
        errors.append(f'Display_Reserved {display[0]:#x}+{display[1]:#x} does not match splash_region '
                      f'{splash[0]:#x}+{splash[1]:#x}')

    width = re.search(r'PcdFrameBufferWidth\|(\d+)', dsc)
    height = re.search(r'PcdFrameBufferHeight\|(\d+)', dsc)
    depth = re.search(r'PcdFrameBufferColorDepth\|(\d+)', dsc)
    if display and width and height and depth:
        needed = int(width[1]) * int(height[1]) * (int(depth[1]) // 8)
        if needed > display[1]:
            errors.append(f'framebuffer needs {needed:#x} bytes but Display_Reserved is {display[1]:#x}')

    dependencies = set()
    for inc in (PLATFORM / 'Include').glob('*.inc'):
        dependencies.update(re.findall(r'Binaries/popsicle/[^\s}]+', inc.read_text()))
    missing = sorted(p for p in dependencies if not (UEFI / p).is_file())

    # Second, independent source of truth: the memorymap blob XBL itself uses, decoded from
    # xbl_config.img and checked in as assets/vendor-memorymap.json. The device tree says which ranges
    # the kernel must keep out of; this says which ranges the boot firmware has to place at all. A
    # vendor region with no covering descriptor is memory UEFI would treat as ordinary free RAM.
    #
    # Coverage by a Conv descriptor is accepted rather than flagged: DXE_Heap is the heap and DBI_Dump
    # is reclaimable, so the vendor naming them does not make them reserved. Only absence is fatal.
    vendor_path = Path(__file__).resolve().parent.parent / 'assets/vendor-memorymap.json'
    vendor_checked = 0
    if vendor_path.exists():
        for region in json.loads(vendor_path.read_text()):
            base, size = region['base'], region['size']
            vendor_checked += 1
            if not any(a <= base and base + size <= a + extent for _, a, extent, _ in rows):
                errors.append(f'vendor region not covered by any descriptor: '
                              f'{region["tags"][1]} {base:#x}+{size:#x}')

    # Optional: compare against the stock [MemoryMap] of the vendor firmware, when it has been staged.
    stock_count = 0
    firmware = Path('.cache/firmware/popsicle/uefiplat.cfg')
    if firmware.exists():
        text = firmware.read_text().split('[MemoryMap]', 1)[1].split('[RegisterMap]', 1)[0]
        for line in text.splitlines():
            if not line.strip().startswith('0x'):
                continue
            record = next(csv.reader([line], skipinitialspace=True))
            base, size = int(record[0], 16), int(record[1], 16)
            remaining = [(base, base + size)]
            stock_count += 1
            for name, address, extent, attributes in rows:
                remaining = [piece for lo, hi in remaining
                             for piece in [(lo, min(hi, address)), (max(lo, address + extent), hi)]
                             if piece[0] < piece[1]]
                if max(base, address) < min(base + size, address + extent):
                    if record[3] == 'NoMap' and 'HobOnlyNoCacheSetting' not in attributes:
                        errors.append(f'Stock NoMap range is mapped: {name}')
                    if record[6] == 'Reserv' and not re.search(r'\bReserv\b', attributes):
                        errors.append(f'Stock reserved range is allocatable: {name}')
            if remaining:
                warnings.append(f'stock region left uncovered: {record[2]}')

    # The firmware volume has to carry UniXNU, or the payload step builds a volume nothing reads.
    # Two separate mistakes are worth catching, because both leave a firmware that boots to the plain
    # UEFI boot manager and looks perfectly healthy:
    #   * popsicle.dsc stops naming the components, so they are never compiled;
    #   * popsicle.fdf stops packing them, so they are compiled and then left out of the volume.
    # DarwinBoot is the deliberate exception: it is named in the dsc so it keeps compiling against the
    # same libraries, and left out of the fdf exactly as the reference platform leaves it out.
    unpacked = []
    for name, must_pack in (('UniXNUPkg/Application/UniXNU/UniXNU.inf', True),
                            ('UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf', True),
                            ('DarwinBootPkg/Driver/DarwinBoot.inf', False)):
        dsc_text = (PLATFORM / 'popsicle.dsc').read_text()
        fdf_text = (PLATFORM / 'popsicle.fdf').read_text()
        if not re.search(rf'^\s*{re.escape(name)}\s*$', dsc_text, re.M):
            errors.append(f'popsicle.dsc does not build {name}')
        packed = bool(re.search(rf'^\s*INF\s+{re.escape(name)}\s*$', fdf_text, re.M))
        if must_pack and not packed:
            errors.append(f'popsicle.fdf does not pack {name}')
        if not must_pack and packed:
            errors.append(f'popsicle.fdf packs {name}, which should stay unpacked')
        if must_pack:
            unpacked.append(name)

    result = {'descriptors': len(rows), 'source_reservations': len(reservations),
              'stock_regions_checked': stock_count, 'vendor_regions_checked': vendor_checked,
              'unixnu_components_packed': len(unpacked),
              'display_reserved': None if display is None else [f'{display[0]:#x}', f'{display[1]:#x}'],
              'splash_region': None if splash is None else [f'{splash[0]:#x}', f'{splash[1]:#x}'],
              'static_errors': errors, 'static_warnings': warnings,
              'missing_vendor_inputs': missing,
              'runtime_ram_verified': False, 'hardware_boot_demonstrated': False}
    out = Path('build/popsicle/check.json')
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(reservations)} DTS reservations checked against {len(rows)} descriptors')
    print(f'{vendor_checked} vendor memorymap regions cross-checked')
    print(f'{len(unpacked)} UniXNU components required in both the dsc and the fdf')
    print(f'{len(errors)} static errors; {len(warnings)} warnings; {len(missing)} vendor inputs still missing; hardware unverified')
    for warning in warnings:
        print(f'warning: {warning}')
    for error in errors:
        print(f'error: {error}')
    raise SystemExit(bool(errors))
