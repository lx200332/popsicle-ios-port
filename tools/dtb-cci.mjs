import { readFileSync } from 'node:fs';
import { parseFdt, fmtVal } from './fdt.mjs';

const buf = readFileSync('C:/dsh-tools/cache/popsicle.dtb');
const t = parseFdt(buf);
const root = t.root.children[0];
function find(n, path, name) {
  for (const c of n.children) {
    const p = path + '/' + c.name;
    if (c.name === name) return c;
    const r = find(c, p, name);
    if (r) return r;
  }
  return null;
}
// dump subtree helper
function dump(n, indent) {
  for (const p of n.props) {
    console.log(`${indent}${p.name} = ${fmtVal(p.value, 200)}`);
  }
  for (const c of n.children) {
    console.log(`${indent}${c.name} {`);
    dump(c, indent + '  ');
    console.log(`${indent}}`);
  }
}
for (const cci of ['cci0@941b000','cci1@941c000','cci2@941d000']) {
  const n = find(root, '', cci);
  console.log(`===== ${cci} =====`);
  if (n) dump(n, '');
}
