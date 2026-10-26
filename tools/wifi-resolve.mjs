import { readFileSync } from 'node:fs';
import { parseFdt, fmtVal } from './fdt.mjs';

const file = process.argv[2];
const buf = readFileSync(file);
const t = parseFdt(buf);
const root = t.root.children[0] || t.root;

// phandle -> {path, node}
const ph = new Map();
function walk(node, path) {
  const p = path + '/' + node.name;
  for (const pr of node.props) {
    if (pr.name === 'phandle' && pr.value.length === 4) {
      ph.set(pr.value.readUInt32BE(0), { path: p, node });
    }
  }
  for (const c of node.children) walk(c, p);
}
for (const c of root.children) walk(c, '');

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
function show(node) {
  for (const pr of node.props) console.log(`    ${pr.name} = ${fmtVal(pr.value, 48)}`);
}
function resolveCell(v, idx) { return v.readUInt32BE(idx * 4); }

console.log(`# ${file}`);
// 1. cnss-peach supplies & clocks & gpios
const peach = find('soc/qcom,cnss-peach@b0000000');
if (peach) {
  console.log('\n## qcom,cnss-peach@b0000000 resolved refs');
  for (const pr of peach.props) {
    const v = pr.value;
    if (pr.name === 'clocks' && v.length >= 8) {
      const p1 = ph.get(resolveCell(v, 0)), id = resolveCell(v, 1);
      console.log(`    clocks -> ${p1 ? p1.path : '?'} clk#${id}`);
      const cn = p1 && p1.node.props.find(x => x.name === 'clock-output-names');
      if (cn) console.log(`      output-names: ${fmtVal(cn.value)}`);
      const ids = p1 && p1.node.props.find(x => x.name === 'clock-ids');
      // clock-id tables: #clock-cells often 1 with clock-ids or not present
    } else if (pr.name.endsWith('-supply') && v.length === 4) {
      const p1 = ph.get(resolveCell(v, 0));
      console.log(`    ${pr.name} -> ${p1 ? p1.path : '?'}`);
      if (p1) {
        const reg = p1.node.props.find(x => x.name === 'reg');
        const rn = p1.node.props.find(x => x.name === 'regulator-name');
        const com = p1.node.props.find(x => x.name === 'compatible');
        console.log(`      reg=${reg ? fmtVal(reg.value, 4) : '-'} name=${rn ? fmtVal(rn.value) : '-'} compatible=${com ? fmtVal(com.value) : '-'}`);
      }
    } else if (pr.name.includes('gpio') && v.length === 12) {
      const p1 = ph.get(resolveCell(v, 0)), pin = resolveCell(v, 1), flags = resolveCell(v, 2);
      console.log(`    ${pr.name} -> ${p1 ? p1.path : '?'} pin#${pin} flags=0x${flags.toString(16)}`);
    } else if (pr.name === 'nvmem-cells' && v.length === 4) {
      const p1 = ph.get(resolveCell(v, 0));
      console.log(`    nvmem-cells -> ${p1 ? p1.path : '?'}`);
      if (p1) show(p1.node);
    }
  }
}

// 2. cnss_pins mux details (pins used)
console.log('\n## cnss_pins pinctrl (pins/groups)');
const pinct = find('soc/pinctrl@f000000');
if (pinct) {
  const sub = pinct.children.find(c => c.name === 'cnss_pins');
  if (sub) {
    for (const c of sub.children) {
      console.log(`  == ${c.name} ==`);
      for (const m of c.children) {
        console.log(`    [${m.name}]`);
        show(m);
      }
    }
  }
}

// 3. pcie0 mux pins
console.log('\n## pcie0 pinctrl');
if (pinct) {
  const sub = pinct.children.find(c => c.name === 'pcie');
  if (sub) {
    for (const c of sub.children) {
      console.log(`  == ${c.name} ==`);
      for (const m of c.children) { console.log(`    [${m.name}]`); show(m); }
    }
  }
}

// 4. reserved-memory cnss region addr
console.log('\n## cnss_wlan_region (parent reserved-memory)');
const rm = find('reserved-memory');
if (rm) {
  for (const c of rm.children) {
    if (/cnss|wlan/i.test(c.name)) { console.log(`  == ${c.name} ==`); show(c); }
  }
}

// 5. TLMM node identity (phandle 0x30 resolution above), pmic gpio for bt-en
console.log('\n## phandle 0x30 / 0xa4 / 0x34 / 0x47 / 0x6b / 0x383 / 0x19 / 0x36 / 0x6c identity');
for (const h of [0x30, 0xa4, 0x34, 0x47, 0x6b, 0x383, 0x19, 0x36, 0x6c, 0x9]) {
  const e = ph.get(h);
  console.log(`  phandle 0x${h.toString(16)} -> ${e ? e.path : 'NOT FOUND'}`);
}

// 6. qrtr-mhi-cnss + vendor-wlan-wonder + direct-link
for (const p of ['soc/qcom,qrtr-mhi-cnss', 'soc/qcom,cnss-vendor-wlan-wonder', 'soc/qcom,cnss-direct-link']) {
  const n = find(p);
  if (n) { console.log(`\n## ${p}`); show(n); }
}
