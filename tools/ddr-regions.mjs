// Dump /ddr-regions and /mem-offline in full: these are the vendor's own DRAM topology nodes.
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
      cur = { name, path, props: {}, order: Object.keys(nodes).length };
      nodes.push(cur);
      stack.push(path);
    } else if (tok === FDT_END_NODE) {
      stack.pop();
      cur = null;
    } else if (tok === FDT_PROP) {
      const len = buf.readUInt32BE(p);
      const nameoff = buf.readUInt32BE(p + 4);
      p += 8;
      const value = Buffer.from(buf.subarray(p, p + len));
      if (cur) cur.props[str(nameoff)] = value;
      p = (p + len + 3) & ~3;
    } else if (tok === FDT_NOP) continue;
    else if (tok === FDT_END) break;
    else throw new Error(`bad token ${tok}`);
  }
  return nodes;
}

const cells = (b) => [...Array(b.length / 4)].map((_, i) => '0x' + b.readUInt32BE(i * 4).toString(16));
const show = (n) => {
  console.log(`  ${n.path}`);
  for (const [k, v] of Object.entries(n.props)) {
    if (k === 'phandle') continue;
    const printable = v.length && [...v].every(c => c === 0 || (c >= 0x20 && c < 0x7f));
    if (printable && v.length > 1) {
      console.log(`    ${k} = "${v.toString('utf8').replace(/\0.*$/s, '')}"`);
    } else if (v.length % 4 === 0 && v.length > 0) {
      console.log(`    ${k} = <${cells(v).join(' ')}>`);
    } else {
      console.log(`    ${k} = [${[...v].map(c => c.toString(16).padStart(2, '0')).join(' ')}]`);
    }
  }
};

const all = parse(readFileSync(process.argv[2] || 'dts/popsicle_merged.dtb'));

for (const top of ['/ddr-regions', '/mem-offline', '/memory']) {
  console.log(`=== ${top} ===`);
  const hits = all.filter(n => n.path === top || n.path.startsWith(top + '/'));
  if (!hits.length) console.log('  (absent)');
  for (const n of hits) show(n);
  console.log();
}

// reserved-memory children whose name looks like ddr / memory / ram
console.log('=== /reserved-memory children named like ddr/memory ===');
for (const n of all.filter(n => n.path.startsWith('/reserved-memory/') && !n.path.slice(18).includes('/'))) {
  if (/ddr|memory|ram/i.test(n.name)) show(n);
}
