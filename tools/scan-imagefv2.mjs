// Look at what immediately follows the imagefv_a volume header, and hunt for compression signatures.
import fs from 'node:fs';

const d = fs.readFileSync('C:\\dsh-对话\\popsicle-port\\fw\\dec\\imagefv_a_fv0.dec');
const hex = (buf, n = 16) => [...buf.subarray(0, n)].map(b => b.toString(16).padStart(2, '0')).join(' ');

console.log('=== FV header region 0x00..0x90 ===');
for (let off = 0; off < 0x90; off += 16) {
  console.log(`0x${off.toString(16).padStart(4, '0')}  ${hex(d.subarray(off, off + 16))}`);
}

console.log('\n=== bytes right after header (0x48..0xa0) ===');
for (let off = 0x48; off < 0xa0; off += 16) {
  console.log(`0x${off.toString(16).padStart(4, '0')}  ${hex(d.subarray(off, off + 16))}`);
}

// count zero runs and look for the EFI standard compression section header (type 0x01, size, 0x01, uncompressed len)
console.log('\n=== signature hunt ===');
const patterns = {
  'LZMA alone (5d 00 00)': [0x5d, 0x00, 0x00],
  'LZMA/EFI (00 00 00 00 5d)': [0x5d],
  'Tiano (compression type 1)': null,
  'FFS free space (ff ff ff)': [0xff, 0xff, 0xff],
};
for (const [name, pat] of Object.entries(patterns)) {
  if (!pat) continue;
  let n = 0, firstAt = -1;
  for (let i = 0; i + pat.length <= d.length; i++) {
    let ok = true;
    for (let k = 0; k < pat.length; k++) if (d[i + k] !== pat[k]) { ok = false; break; }
    if (ok) { n++; if (firstAt < 0) firstAt = i; }
  }
  console.log(`  ${name}: ${n} hits, first at ${firstAt >= 0 ? '0x' + firstAt.toString(16) : 'none'}`);
}

// where does the first long zero run start after the header?
let z = 0x48;
while (z < d.length && d[z] === 0xff) z++;
console.log(`\nfirst non-0xff byte after header: 0x${z.toString(16)} (value 0x${d[z].toString(16)})`);

// Walk FFS assuming header starts at 0x48 and see where it breaks.
console.log('\n=== manual FFS walk from 0x48 ===');
let off = 0x48;
for (let i = 0; i < 12; i++) {
  if (off + 24 > d.length) { console.log('  out of range'); break; }
  const hdrByte = d[off + 16];
  const type = d[off + 18];
  const attrs = d[off + 19];
  const size = d[off + 20] | (d[off + 21] << 8) | (d[off + 22] << 16);
  console.log(`  @0x${off.toString(16)} guid=${d.subarray(off, off + 16).toString('hex')} hdr=0x${hdrByte.toString(16)} type=${type} attr=0x${attrs.toString(16)} size=${size} (0x${size.toString(16)})`);
  if (hdrByte !== 0xaa && hdrByte !== 0x5a) { console.log('    -> not an FFS header, stop'); break; }
  if (size < 24) { console.log('    -> bad size, stop'); break; }
  off += size;
  off = (off + 7) & ~7;
}
