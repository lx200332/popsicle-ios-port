// Dump every property of the popsicle primary/secondary panel nodes from the merged tree.
import fs from 'node:fs';

const TARGETS = [
  'qcom,mdss_dsi_p2_42_0d_0b_mp_prim_dsc_cmd',
  'qcom,mdss_dsi_p2_42_06_0a_sec_dsc_cmd',
];
const d = fs.readFileSync('C:\\dsh-对话\\popsicle-port\\dts\\popsicle_merged.dtb');
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

const fmt = (name, v) => {
  if (v.length === 0) return '<empty>';
  if (v.length === 4 && !/command|sequence/i.test(name)) return String(v.readUInt32BE(0));
  if (/name|type|model|mode|regulator|supply|prefix|suffix/i.test(name)) {
    const s = v.toString('latin1').replace(/\0/g, '|').replace(/\|+$/, '');
    if (/^[\x20-\x7e|]+$/.test(s)) return JSON.stringify(s);
  }
  // byte array: show as hex, truncated
  const hex = [...v].map(b => b.toString(16).padStart(2, '0')).join(' ');
  return hex.length > 200 ? hex.slice(0, 200) + ` … (${v.length} bytes)` : `${hex}  (${v.length} bytes)`;
};

for (const t of TARGETS) {
  const n = nodes.find(x => x.name === t);
  console.log(`\n########## ${t} ##########`);
  if (!n) { console.log('  NOT FOUND'); continue; }
  console.log(`path: ${n.path}`);
  const keys = Object.keys(n.props).sort();
  console.log(`properties: ${keys.length}`);
  for (const k of keys) console.log(`  ${k} = ${fmt(k, n.props[k])}`);
}
