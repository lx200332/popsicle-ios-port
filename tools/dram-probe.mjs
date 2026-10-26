// Dump every node that could describe physical DRAM: memory@*, anything named ddr, the /memory
// node, and the bootargs the bootloader passed. Independent FDT reader (the real root is the
// synthetic wrapper's first child).
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
    const start = offStrings + off;
    let end = start;
    while (end < offStrings + sizeStrings && buf[end] !== 0) end++;
    return buf.toString('utf8', start, end);
  };

  while (p < totalsize) {
    const tok = buf.readUInt32BE(p); p += 4;
    if (tok === FDT_BEGIN_NODE) {
      let end = p;
      while (buf[end] !== 0) end++;
      const name = buf.toString('utf8', p, end);
      p = (end + 4) & ~3;
      const parent = stack.length ? stack[stack.length - 1] : null;
      const path = parent === null ? (name === '' ? '/' : `/${name}`)
        : (parent === '/' ? `/${name}` : `${parent}/${name}`);
      cur = { name, path, props: {}, children: [], parent };
      nodes.push(cur);
      if (parent !== null) nodes.find(n => n.path === parent)?.children.push(path);
      stack.push(path);
    } else if (tok === FDT_END_NODE) {
      stack.pop();
      cur = stack.length ? nodes.find(n => n.path === stack[stack.length - 1]) : null;
    } else if (tok === FDT_PROP) {
      const len = buf.readUInt32BE(p);
      const nameoff = buf.readUInt32BE(p + 4);
      p += 8;
      const value = buf.subarray(p, p + len);
      if (cur) cur.props[str(nameoff)] = value;
      p = (p + len + 3) & ~3;
    } else if (tok === FDT_NOP) {
      continue;
    } else if (tok === FDT_END) {
      break;
    } else {
      throw new Error(`bad token ${tok} at ${p - 4}`);
    }
  }
  return nodes;
}

const dtbPath = process.argv[2] || 'dts/popsicle_merged.dtb';
const all = parse(readFileSync(dtbPath));
const root = all[0];
console.log(`root: ${root.name || '(unnamed)'}  path=${root.path}  total nodes=${all.length}`);
console.log(`root props: ${Object.keys(root.props).join(', ')}`);
console.log();

// 1. every node whose name or path mentions memory / ddr / ram
console.log('=== nodes mentioning memory / ddr / ram ===');
for (const n of all) {
  if (/mem|ddr|ram/i.test(n.name) && !/reserved-memory/.test(n.path)) {
    const reg = n.props['reg'];
    const dt = n.props['device_type'];
    const size = n.props['size'] ?? n.props['qcom,ddr-size'];
    if (reg || dt || size) {
      const cells = reg ? [...Array(reg.length / 4)].map((_, i) => '0x' + reg.readUInt32BE(i * 4).toString(16)) : [];
      console.log(`  ${n.path}`);
      if (dt) console.log(`    device_type = ${dt.toString('utf8').replace(/\0.*$/s, '')}`);
      if (reg) console.log(`    reg = <${cells.join(' ')}>`);
      if (size) console.log(`    size = ${[...Array(size.length / 4)].map((_, i) => '0x' + size.readUInt32BE(i * 4).toString(16)).join(' ')}`);
    }
  }
}
console.log();

// 2. root children, so we can see what the top level looks like
console.log('=== root children ===');
for (const c of root.children) console.log(`  ${c}`);
console.log();

// 3. bootargs
const chosen = all.find(n => n.path === '/chosen');
if (chosen?.props['bootargs']) {
  const args = chosen.props['bootargs'].toString('utf8').replace(/\0.*$/s, '');
  console.log(`=== /chosen/bootargs (${args.length} chars) ===`);
  for (const tok of args.split(/\s+/)) {
    if (/mem|ddr|ram|size/i.test(tok)) console.log(`  ${tok}`);
  }
}
console.log();

// 4. what the vendor map covers, to see whether the low 2 GiB is fully backed
const vm = JSON.parse(readFileSync('UniXNU-popsicle/assets/vendor-memorymap.json', 'utf8'));
const regions = (vm.regions ?? vm).map(r => ({
  name: r.name,
  base: typeof r.base === 'string' ? BigInt(r.base) : BigInt(r.base),
  size: typeof r.size === 'string' ? BigInt(r.size) : BigInt(r.size),
}));
regions.sort((a, b) => (a.base < b.base ? -1 : a.base > b.base ? 1 : 0));
console.log(`=== vendor map: ${regions.length} regions ===`);
const lo = regions[0], hi = regions.reduce((m, r) => (r.base + r.size > m.base + m.size ? r : m));
console.log(`  lowest  ${lo.name} 0x${lo.base.toString(16)}`);
console.log(`  highest ${hi.name} 0x${hi.base.toString(16)} + 0x${hi.size.toString(16)} = 0x${(hi.base + hi.size).toString(16)}`);
const inWindow = regions.filter(r => r.base >= 0x80000000n && r.base + r.size <= 0x100000000n);
const covered = inWindow.reduce((s, r) => s + r.size, 0n);
console.log(`  regions fully inside 0x80000000..0x100000000: ${inWindow.length}, covering 0x${covered.toString(16)} = ${Number(covered) / 2 ** 20} MiB of 2048 MiB`);
const above = regions.filter(r => r.base >= 0x100000000n);
console.log(`  regions above 4 GiB: ${above.length}${above.length ? ' -> ' + above.map(r => `${r.name} 0x${r.base.toString(16)}+0x${r.size.toString(16)}`).join(', ') : ''}`);
