import { readFileSync } from 'node:fs';
import { parseFdt, fmtVal } from './fdt.mjs';

const file = process.argv[2];
const buf = readFileSync(file);
const t = parseFdt(buf);
const root = t.root.children[0] || t.root;

const ph = new Map();
function walk(node, path) {
  const p = path + '/' + node.name;
  for (const pr of node.props) {
    if (pr.name === 'phandle' && pr.value.length === 4) ph.set(pr.value.readUInt32BE(0), node);
  }
  for (const c of node.children) walk(c, p);
}
for (const c of root.children) walk(c, '');

function dump(node, label) {
  console.log(`\n### ${label}`);
  const keys = ['compatible', 'reg', 'reg-names', '#gpio-cells', 'gpio-controller', '#clock-cells', 'clock-output-names', 'label', 'device_type', 'status', 'phandle'];
  for (const k of keys) {
    const p = node.props.find(x => x.name === k);
    if (p) console.log(`  ${k} = ${fmtVal(p.value, 16)}`);
  }
  console.log(`  name = "${node.name}"`);
}

// who is phandle 0x30 (TLMM?) and 0x34 (rf_clk source?)
for (const h of [0x30, 0x34]) {
  const n = ph.get(h);
  if (n) dump(n, `phandle 0x${h.toString(16)}`);
  else console.log(`phandle 0x${h.toString(16)} NOT FOUND`);
}

// main TLMM
function find(path) {
  const parts = path.split('/').filter(Boolean);
  let cur = root;
  for (const seg of parts) {
    const nxt = cur.children.find(c => c.name === seg);
    if (!nxt) return null;
    cur = nxt;
  }
  return cur;
}
const tlmm = find('soc/pinctrl@f000000');
if (tlmm) dump(tlmm, 'soc/pinctrl@f000000 (TLMM)');

const gcc = find('soc/clock-controller@100000');
if (gcc) dump(gcc, 'soc/clock-controller@100000 (GCC)');

// list all clock-controller nodes
console.log('\n### all clock-controller / phandle owners');
(function listClock(n, path) {
  const p = path + '/' + n.name;
  if (/clock-controller|rpmhcc|videocc|camcc|dispcc|gpucc|tcsrcc|aop/i.test(n.name)) {
    const phd = n.props.find(x => x.name === 'phandle');
    console.log(`  ${p}  phandle=${phd ? '0x' + phd.value.readUInt32BE(0).toString(16) : '-'}`);
  }
  for (const c of n.children) listClock(c, p);
})(root, '');

// all pinctrl nodes
console.log('\n### all pinctrl nodes');
(function listPin(n, path) {
  const p = path + '/' + n.name;
  if (/pinctrl/.test(n.name)) {
    const phd = n.props.find(x => x.name === 'phandle');
    console.log(`  ${p}  phandle=${phd ? '0x' + phd.value.readUInt32BE(0).toString(16) : '-'}`);
  }
  for (const c of n.children) listPin(c, p);
})(root, '');

// any node with phandle 0x34 value conflicts? scan all phandle props count
let cnt = 0;
(function scan(n) {
  for (const pr of n.props) if (pr.name === 'phandle') cnt++;
  for (const c of n.children) scan(c);
})(root);
console.log(`\ntotal phandle props scanned: ${cnt}`);
