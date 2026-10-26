import { readFileSync } from 'node:fs';
import { parseFdt, fmtVal } from './fdt.mjs';

const file = process.argv[2];
const buf = readFileSync(file);
const t = parseFdt(buf);
const root = t.root.children[0] || t.root;

const map = new Map(); // phandle -> {ph, node, path} (last wins)
function walk(node, path) {
  const p = path + '/' + node.name;
  for (const pr of node.props) {
    if (pr.name === 'phandle' && pr.value.length === 4) {
      map.set(pr.value.readUInt32BE(0), { ph: pr.value.readUInt32BE(0), node, path: p });
    }
  }
  for (const c of node.children) walk(c, p);
}
for (const c of root.children) walk(c, '');

function findByReg(regStr) {
  const out = [];
  const target = regStr.toLowerCase();
  for (const [h, e] of map) {
    const reg = e.node.props.find(x => x.name === 'reg');
    if (!reg) continue;
    let found = false;
    for (let i = 0; i + 8 <= reg.value.length; i += 8) {
      const hi = reg.value.readUInt32BE(i), lo = reg.value.readUInt32BE(i + 4);
      const s = `0x${hi.toString(16)} 0x${lo.toString(16)}`;
      if (s.toLowerCase().includes(target)) { found = true; break; }
    }
    if (found) out.push(`  ${e.path}  (phandle 0x${h.toString(16)})  compatible=${e.node.props.find(x=>x.name==='compatible') ? fmtVal(e.node.props.find(x=>x.name==='compatible').value) : '-'}`);
  }
  return out;
}

console.log('## owners of reg "0x0 0xb0000000":');
for (const l of findByReg('0x0 0xb0000000')) console.log(l);
console.log('## owners of reg containing "f000000" (TLMM + pmic pinctrl base):');
for (const l of findByReg('0xf000000')) console.log(l);
console.log('## owners of reg containing "8800":');
for (const l of findByReg('0x8800')) console.log(l);
console.log('## owners of reg containing "100000":');
for (const l of findByReg('0x100000')) console.log(l);
console.log('## owners of reg containing "c400000":');
for (const l of findByReg('0xc400000')) console.log(l);

// thermal-sensors phandle 0x9 identity
const n9 = map.get(0x9);
console.log('\n## phandle 0x9 (thermal-sensors parent):');
if (n9) {
  console.log(`  ${n9.path}`);
  for (const pr of n9.node.props) if (['compatible','reg','#thermal-sensor-cells'].includes(pr.name)) console.log(`    ${pr.name} = ${fmtVal(pr.value, 8)}`);
} else console.log('  NOT FOUND');
