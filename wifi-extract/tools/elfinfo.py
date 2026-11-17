import struct, os, re, sys

def parse(fn):
    d = open(fn, 'rb').read()
    print("\n=== %s  size=%d" % (fn, len(d)))
    if d[:4] != b'\x7fELF':
        ss = [m.group().decode('latin1') for m in re.finditer(rb'[ -~]{8,}', d)]
        print("  not ELF, strings=%d" % len(ss))
        for s in ss[:10]: print("   ", s[:110])
        return
    cls = d[4]
    if cls == 1:
        (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
         e_ehsize, e_phentsize, e_phnum) = struct.unpack('<HHIIIIIHHH', d[16:46])
        print("  ELF32 machine=0x%x type=%d entry=0x%08x phoff=0x%x phnum=%d flags=0x%x"
              % (e_machine, e_type, e_entry, e_phoff, e_phnum, e_flags))
        for i in range(min(e_phnum, 8)):
            off = e_phoff + i * 32
            (pt, poff, pva, ppa, pfsz, pmsz, pfl, pal) = struct.unpack('<IIIIIIII', d[off:off+32])
            print("   ph%d type=%d off=0x%x vaddr=0x%08x filesz=0x%x memsz=0x%x flags=%d"
                  % (i, pt, poff, pva, pfsz, pmsz, pfl))
    ss = [m.group().decode('latin1') for m in re.finditer(rb'[ -~]{8,}', d)]
    key = [s for s in ss if re.search(r'(?i)wlan|amss|peach|cnss|build|version|qca|rom:|bdf|cal|board|hw_', s)]
    print("  strings total=%d key=%d" % (len(ss), len(key)))
    for s in key[:14]: print("    ", s[:110])

for fn in sys.argv[1:]:
    if os.path.exists(fn): parse(fn)
