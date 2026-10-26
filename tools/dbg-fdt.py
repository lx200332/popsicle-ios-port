import struct, sys
from pathlib import Path

data = Path(r'<WORKSPACE>\popsicle-port\dts\popsicle_merged.dtb').read_bytes()
magic, total, off_struct, size_struct, off_strings, size_strings = struct.unpack_from('>6I', data, 0)
print(f'magic={magic:#x} total={total} off_struct={off_struct:#x} size_struct={size_struct:#x} off_strings={off_strings:#x} size_strings={size_strings:#x}')

def s_at(off):
    end = data.index(b'\0', off)
    return data[off:end].decode('utf-8', 'replace')

stack = []
cur = off_struct
depth_report = []
seen = 0
while True:
    (tok,) = struct.unpack_from('>I', data, cur)
    cur += 4
    if tok == 1:
        name = s_at(cur)
        cur += (len(name.encode()) + 1 + 3) & ~3
        stack.append(name or '/')
        if len(stack) <= 3:
            depth_report.append((len(stack), list(stack)))
    elif tok == 2:
        if stack: stack.pop()
    elif tok == 3:
        length, noff = struct.unpack_from('>II', data, cur)
        cur += 8
        pname = s_at(off_strings + noff)
        cur += (length + 3) & ~3
        seen += 1
        if len(stack) >= 2 and stack[1] == 'reserved-memory':
            depth_report.append(('PROP', len(stack), list(stack), pname, length))
    elif tok == 4:
        continue
    elif tok == 9:
        break
    else:
        print('unknown token', tok, hex(cur-4)); break

print('props seen:', seen)
for r in depth_report[:40]:
    print(r)
