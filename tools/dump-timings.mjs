// Dump the display-timings subtree of the popsicle primary panel (timings + init sequences).
import fs from 'node:fs';

const PANEL = 'qcom,mdss_dsi_p2_42_0d_0b_mp_prim_dsc_cmd';
const d = fs.readFileSync('C:\\<WORKSPACE>\\popsicle-port\\dts\\popsicle_merged.dtb');
const be32 = o => d.readUInt32BE(o);
const offStruct = be32(8), offStrings = be32(12);

let off = offStruct;
const stack = [];
const nodes = [];
while (off < d.length) {
  const tok = be32(off); off += 4;
  if (tok === 1) {
    let e = off; while (d[e] !== 0) e++;
    const name = d.subarray(off, e).toString('latin1');
    off = (e + 4) & ~3;
    stack.push(name);
    nodes.push({ path: '/' + stack.filter(Boolean).join('/'), name, props: {}, depth: stack.length });
  } else if (tok === 2) stack.pop();
  else if (tok === 3) {
    const len = be32(off), nameOff = be32(off + 4); off += 8;
    let s = offStrings + nameOff, e = s; while (d[e] !== 0) e++;
    const pname = d.subarray(s, e).toString('latin1');
    nodes[nodes.length - 1].props[pname] = d.subarray(off, off + len);
    off = (off + len + 3) & ~3;
  } else if (tok === 4) continue;
  else break;
}

const panel = nodes.find(n => n.name === PANEL);
if (!panel) { console.log('panel not found'); process.exit(1); }
const base = panel.path;
console.log('panel path:', base);

// children = nodes whose path starts with base + '/' and has no deeper panel ancestor
const kids = nodes.filter(n => n.path.startsWith(base + '/'));
console.log('descendant nodes:', kids.length);
for (const k of kids) {
  const rel = k.path.slice(base.length + 1);
  const depth = rel.split('/').length;
  console.log(`${'  '.repeat(depth)}[${k.name}]  ${Object.keys(k.props).length} props`);
}

const fmt = (name, v) => {
  if (v.length === 0) return '<empty>';
  if (v.length === 4 && !/command|sequence/i.test(name)) return String(v.readUInt32BE(0));
  if (/name|type|mode|state|order|flag/i.test(name)) {
    const s = v.toString('latin1').replace(/\0/g, '|').replace(/\|+$/, '');
    if (/^[\x20-\x7e|]+$/.test(s)) return JSON.stringify(s);
  }
  const hex = [...v].map(b => b.toString(16).padStart(2, '0')).join(' ');
  return hex.length > 160 ? hex.slice(0, 160) + ` … (${v.length} bytes)` : `${hex}  (${v.length} bytes)`;
};

// print the timing child in full
const timing = kids.find(k => /display-timings|timing@0/.test(k.name));
if (timing) {
  console.log(`\n===== ${timing.name} (${Object.keys(timing.props).length} props) =====`);
  for (const p of Object.keys(timing.props).sort()) console.log(`  ${p} = ${fmt(p, timing.props[p])}`);
} else {
  console.log('\nno display-timings child; listing names of all children:');
  for (const k of kids) console.log('  ' + k.name);
}
