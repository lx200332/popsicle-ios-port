// Every /reserved-memory child, with its reg and the flags that decide whether it is DRAM or an
// MMIO aperture. `no-map` means "do not map as normal memory"; `reusable`/`compatible` matter too.
import { readFileSync } from 'node:fs';

const FDT_MAGIC = 0xd00dfeed;
const FDT_BEGIN_NODE = 1, FDT_END_NODE = 2, FDT_PROP = 3, FDT_NOP = 4, FDT_END = 9;

function parse(buf) {
  if (buf.readUInt32BE(0) !== FDT_MAGIC) throw new Error('not an FDT');
  const totalsize = buf.readUInt32BE(4);
  const offStruct = buf.readUInt32BE(8);
  const offStrings = buf.readUInt32BE(12);
  const sizeStrings = buf.readUInt32BE(32);
  const nodes = [];
  let p = offStruct;
  const stack = [];
  let cur = null;
  const str = (off) => {
    const s = offStrings + off;
    let e = s;
    while (e < offStrings + sizeStrings && buf[e] !== 0) e++;
    return buf.toString('utf8', s, e);
  };
  while (p < totalsize) {
    const tok = buf.readUInt32BE(p); p += 4;
    if (tok === FDT_BEGIN_NODE) {
      let e = p;
      while (buf[e] !== 0) e++;
      const name = buf.toString('utf8', p, e);
      p = (e + 4) & ~3;
      const parent = stack.length ? stack[stack.length - 1] : null;
      const path = parent === null ? (name === '' ? '/' : `/${name}`)
        : (parent === '/' ? `/${name}` : `${parent}/${name}`);
      cur = { name, path, props: {} };
      nodes.push(cur);
      stack.push(path);
    } else if (tok === FDT_END_NODE) { stack.pop(); cur = null; }
    else if (tok === FDT_PROP) {
      const len = buf.readUInt32BE(p);
      const nameoff = buf.readUInt32BE(p + 4);
      p += 8;
      if (cur) cur.props[str(nameoff)] = Buffer.from(buf.subarray(p, p + len));
      p = (p + len + 3) & ~3;
    } else if (tok === FDT_NOP) continue;
    else if (tok === FDT_END) break;
    else throw new Error(`bad token ${tok}`);
  }
  return nodes;
}

const all = parse(readFileSync(process.argv[2] || 'dts/popsicle_merged.dtb'));
const reg = (v) => v ? [0, 1, 2, 3].map(i => BigInt('0x' + v.readUInt32BE(i * 4).toString(16))) : null;

const rows = [];
for (const n of all) {
  const depth = n.path.split('/').length - 1;
  if (depth !== 2 || !n.path.startsWith('/reserved-memory/')) continue;
  const r = reg(n.props['reg']);
  if (!r) continue;
  const base = (r[0] << 32n) | r[1];
  const size = (r[2] << 32n) | r[3];
  rows.push({
    name: n.name,
    base, size,
    noMap: 'no-map' in n.props,
    reusable: 'reusable' in n.props,
    label: n.props['label']?.toString('utf8').replace(/\0.*$/s, '') ?? '',
  });
}
rows.sort((a, b) => (a.base < b.base ? -1 : a.base > b.base ? 1 : 0));

console.log(`${rows.length} reserved-memory children with a non-zero reg\n`);
const hex = (b) => '0x' + b.toString(16).padStart(9, ' ');
console.log('base          size        no-map reusable  name');
for (const r of rows) {
  console.log(`${hex(r.base)}  ${hex(r.size)}  ${String(r.noMap).padEnd(6)} ${String(r.reusable).padEnd(9)} ${r.name}`);
}

console.log('\n=== regions at or above 4 GiB ===');
const high = rows.filter(r => r.base >= 0x100000000n);
if (!high.length) console.log('  none');
for (const r of high) {
  console.log(`  ${r.name}: 0x${r.base.toString(16)} + 0x${r.size.toString(16)} = 0x${(r.base + r.size).toString(16)}  no-map=${r.noMap} reusable=${r.reusable}`);
}

console.log('\n=== coverage of the 0x80000000..0x100000000 low window ===');
const LO = 0x80000000n, HI = 0x100000000n;
let cursor = LO;
const gaps = [];
for (const r of rows.filter(r => r.base < HI && r.base + r.size > LO)) {
  const s = r.base < LO ? LO : r.base;
  const e = r.base + r.size > HI ? HI : r.base + r.size;
  if (s > cursor) gaps.push([cursor, s]);
  if (e > cursor) cursor = e;
}
if (cursor < HI) gaps.push([cursor, HI]);
const covered = (HI - LO) - gaps.reduce((s, [a, b]) => s + (b - a), 0n);
console.log(`  reserved: 0x${covered.toString(16)} of 0x${(HI - LO).toString(16)} (${Number(covered) / 2 ** 20} MiB of 2048 MiB)`);
for (const [a, b] of gaps) console.log(`  unaccounted: 0x${a.toString(16)} .. 0x${b.toString(16)}  (${Number(b - a) / 2 ** 20} MiB)`);
