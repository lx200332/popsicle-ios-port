// Inspect the decoded imagefv_a volume: locate _FVH headers and FFS file headers.
import fs from 'node:fs';

const p = 'C:\\dsh-对话\\popsicle-port\\fw\\dec\\imagefv_a_fv0.dec';
const d = fs.readFileSync(p);
console.log('file bytes:', d.length, '(0x' + d.length.toString(16) + ')');

// 1. every _FVH signature
const hits = [];
for (let i = 0; i + 4 <= d.length; i++) {
  if (d[i] === 0x5f && d[i+1] === 0x46 && d[i+2] === 0x56 && d[i+3] === 0x48) hits.push(i);
}
console.log('_FVH occurrences:', hits.length, hits.slice(0, 10).map(h => '0x' + h.toString(16)));

for (const h of hits.slice(0, 6)) {
  const base = h - 40;
  if (base < 0) continue;
  const len = d.readUInt32LE(base + 32);
  const fvLen = d.readBigUInt64LE ? Number(d.readBigUInt64LE(base + 32)) : len;
  console.log(`  FVH at 0x${h.toString(16)} base 0x${base.toString(16)} len32=${len} (0x${len.toString(16)}) len64=${fvLen} hdrLen=0x${d.readUInt16LE(base+48).toString(16)} extHdrOff=0x${d.readUInt16LE(base+52).toString(16)} rev=0x${d[base+55].toString(16)}`);
}

// 2. scan for plausible FFS file headers: GUID(16) + u16 header/integrity + u8 type + u8 attrs + u8 size[3]
//    FFS2 uses 0xAA in the header byte. Count by type.
const types = {};
let ffsCount = 0;
const first = [];
for (let i = 0; i + 24 <= d.length; i++) {
  if (d[i+16] !== 0xAA && d[i+16] !== 0x5A) continue;
  const type = d[i+18];
  if (type > 0x20) continue;
  const size = d[i+20] | (d[i+21] << 8) | (d[i+22] << 16);
  if (size < 24 || i + size > d.length) continue;
  // GUID sanity: not all zero, not all ff
  let zero = true, ff = true;
  for (let k = 0; k < 16; k++) { if (d[i+k] !== 0) zero = false; if (d[i+k] !== 0xff) ff = false; }
  if (zero || ff) continue;
  ffsCount++;
  types[type] = (types[type] || 0) + 1;
  if (first.length < 8) first.push({ off: '0x' + i.toString(16), type, size, guid: d.subarray(i, i+16).toString('hex') });
}
console.log('candidate FFS headers:', ffsCount);
console.log('by type:', JSON.stringify(types));
console.log('first:', first);
