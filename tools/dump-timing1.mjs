// Dump one timing node of the popsicle primary panel in full.
import fs from 'node:fs';

const PANEL = 'qcom,mdss_dsi_p2_42_0d_0b_mp_prim_dsc_cmd';
const TIMING = process.argv[2] || 'timing@wqhd_normal_120hz_index_00';
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
    nodes.push({ path: '/' + stack.filter(Boolean).join('/'), name, props: {} });
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

const t = nodes.find(n => n.name === TIMING);
if (!t) { console.log('timing not found:', TIMING); process.exit(1); }
console.log(`===== ${t.path} (${Object.keys(t.props).length} props) =====\n`);

const fmt = (name, v) => {
  if (v.length === 0) return '<empty>';
  if (v.length === 4 && !/command|sequence|init/i.test(name)) return String(v.readUInt32BE(0));
  if (/name|type|mode|state|order|flag/i.test(name) && !/command/i.test(name)) {
    const s = v.toString('latin1').replace(/\0/g, '|').replace(/\|+$/, '');
    if (/^[\x20-\x7e|]+$/.test(s)) return JSON.stringify(s);
  }
  const hex = [...v].map(b => b.toString(16).padStart(2, '0')).join(' ');
  return hex.length > 300 ? hex.slice(0, 300) + ` … (${v.length} bytes)` : `${hex}  (${v.length} bytes)`;
};

for (const p of Object.keys(t.props).sort()) {
  console.log(`${p}\n    ${fmt(p, t.props[p])}`);
}
