// List the display/panel-related node names and compatible strings in the merged popsicle tree.
import fs from 'node:fs';

const d = fs.readFileSync('C:\\<WORKSPACE>\\popsicle-port\\dts\\popsicle_merged.dtb');
const FDT_BEGIN_NODE = 1, FDT_END_NODE = 2, FDT_PROP = 3, FDT_NOP = 4, FDT_END = 9;
const be32 = o => d.readUInt32BE(o);
const offStruct = be32(8), offStrings = be32(12);

let off = offStruct;
const stack = [];
const nodes = [];
while (off < d.length) {
  const tok = be32(off); off += 4;
  if (tok === FDT_BEGIN_NODE) {
    let end = off; while (d[end] !== 0) end++;
    const name = d.subarray(off, end).toString('latin1');
    off = (end + 4) & ~3;
    stack.push(name);
    nodes.push({ path: '/' + stack.filter(Boolean).join('/'), props: {} });
  } else if (tok === FDT_END_NODE) {
    stack.pop();
  } else if (tok === FDT_PROP) {
    const len = be32(off); const nameOff = be32(off + 4); off += 8;
    let s = offStrings + nameOff; let e = s; while (d[e] !== 0) e++;
    const pname = d.subarray(s, e).toString('latin1');
    const val = d.subarray(off, off + len);
    off = (off + len + 3) & ~3;
    nodes[nodes.length - 1].props[pname] = val;
  } else if (tok === FDT_NOP) continue;
  else break;
}

const panels = nodes.filter(n => /panel|dsi|mdss/i.test(n.path) && Object.keys(n.props).length);
console.log('panel/dsi nodes:', panels.length);
const names = new Set();
for (const n of panels) {
  const base = n.path.split('/').pop();
  if (/panel/i.test(base)) names.add(base);
}
console.log('\n=== distinct panel node names ===');
for (const n of [...names].sort()) console.log('  ' + n);

console.log('\n=== compatibles of panel nodes ===');
const comps = new Set();
for (const n of panels) {
  const c = n.props['compatible'];
  if (c) comps.add(c.toString('latin1').replace(/\0/g, ' ').trim());
}
for (const c of [...comps].sort().slice(0, 40)) console.log('  ' + c);

// the primary panel chosen by the display driver
const chosen = nodes.find(n => n.path === '/chosen');
if (chosen && chosen.props['bootargs']) {
  const args = chosen.props['bootargs'].toString('latin1');
  const m = args.match(/mdss[^ ]*/g);
  if (m) console.log('\nbootargs mdss tokens:', m.slice(0, 5));
}
