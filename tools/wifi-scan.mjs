import { readFileSync } from 'node:fs';
import { parseFdt, fmtVal } from './fdt.mjs';

const file = process.argv[2];
const buf = readFileSync(file);
const t = parseFdt(buf);
const root = t.root.children[0] || t.root; // fdt.mjs 合成壳，真根是 children[0]

const re = /wlan|cnss|icnss|ath|wpss|wifi|pcie|qca/i;
const hits = [];
function walk(node, path) {
  const p = path + '/' + node.name;
  if (re.test(node.name)) hits.push({ node, path: p });
  for (const c of node.children) walk(c, p);
}
for (const c of root.children) walk(c, '');

console.log(`# file=${file} header.totalsize=${t.header.totalsize}`);
for (const h of hits) {
  console.log(`\n== NODE ${h.path} ==`);
  for (const pr of h.node.props) {
    const v = fmtVal(pr.value, 64);
    console.log(`  ${pr.name} = ${v}`);
  }
  if (h.node.children.length) {
    console.log(`  children(${h.node.children.length}): ${h.node.children.map(c => c.name).join(', ')}`);
  }
}
console.log(`\n# total matches: ${hits.length}`);
