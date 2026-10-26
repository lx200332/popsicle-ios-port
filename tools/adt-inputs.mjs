// Extract the values the ADT generator needs from a compiled device tree.
import fs from 'node:fs';

const file = process.argv[2];
const buf = fs.readFileSync(file);
const be32 = (o) => buf.readUInt32BE(o);
if (be32(0) !== 0xd00dfeed) throw new Error('bad FDT magic');
const totalsize = be32(4);
const offStruct = be32(8);
const offStrings = be32(12);
const sizeStruct = be32(36);

const cstr = (o) => { let e = o; while (buf[e] !== 0) e++; return buf.toString('utf8', o, e); };

const synthetic = { name: '<synthetic>', props: {}, children: [] };
const stack = [synthetic];
let p = offStruct, done = false;
while (!done) {
  const tok = be32(p); p += 4;
  if (tok === 1) {
    const name = cstr(p);
    p += Math.ceil((Buffer.byteLength(name, 'utf8') + 1) / 4) * 4;
    const node = { name, props: {}, children: [] };
    stack[stack.length - 1].children.push(node);
    stack.push(node);
  } else if (tok === 2) {
    stack.pop();
  } else if (tok === 3) {
    const len = be32(p), nameoff = be32(p + 4); p += 8;
    const pname = cstr(offStrings + nameoff);
    const data = Buffer.from(buf.subarray(p, p + len));
    p += Math.ceil(len / 4) * 4;
    const node = stack[stack.length - 1];
    (node.props[pname] ||= []).push(data);
  } else if (tok === 4) {
    // nop
  } else if (tok === 9) {
    done = true;
  } else {
    throw new Error(`bad token ${tok} at ${p - 4}`);
  }
}
const root = synthetic.children[0];

const hex = (b) => b.toString('hex');
const u32s = (b) => { const out = []; for (let i = 0; i + 4 <= b.length; i += 4) out.push(b.readUInt32BE(i)); return out; };
const hx = (n) => '0x' + n.toString(16);
const str = (node, prop) => { const v = node.props[prop]; return v ? v[0].toString('utf8').replace(/\0+$/, '') : undefined; };

function findChild(node, prefix) {
  return node.children.find((c) => c.name === prefix || c.name.startsWith(prefix + '@'));
}
function collect(node, pred, out = []) {
  if (pred(node)) out.push(node);
  for (const c of node.children) collect(c, pred, out);
  return out;
}
function pathOf(target) {
  const parts = [];
  const walk = (n, trail) => {
    if (n === target) { parts.push(...trail); return true; }
    for (const c of n.children) if (walk(c, [...trail, c.name])) return true;
    return false;
  };
  walk(root, [root.name]);
  return '/' + parts.slice(1).join('/');
}

const addrCells = u32s(root.props['#address-cells']?.[0] ?? Buffer.from([0, 0, 0, 2]))[0];
const sizeCells = u32s(root.props['#size-cells']?.[0] ?? Buffer.from([0, 0, 0, 2]))[0];

function regs(node) {
  const raw = node.props.reg?.[0];
  if (!raw) return [];
  const cells = u32s(raw);
  const per = addrCells + sizeCells;
  const out = [];
  for (let i = 0; i + per <= cells.length; i += per) {
    const addr = cells.slice(i, i + addrCells).reduce((a, v) => (a << 32n) | BigInt(v), 0n);
    const size = cells.slice(i + addrCells, i + per).reduce((a, v) => (a << 32n) | BigInt(v), 0n);
    out.push({ addr, size });
  }
  return out;
}
const regStr = (node) => regs(node).map((r) => `<${hx(r.addr)} ${hx(r.size)}>`).join(' ') || '(none)';

console.log(`file        ${file}`);
console.log(`totalsize   ${totalsize} (${hx(totalsize)})`);
console.log(`root name   ${root.name}`);
console.log(`model       ${str(root, 'model')}`);
console.log(`compatible  ${str(root, 'compatible')}`);
console.log(`addr/size   ${addrCells}/${sizeCells}`);
console.log('');

const cpus = findChild(root, 'cpus');
if (cpus) {
  console.log(`=== /${cpus.name} ===`);
  console.log(`enable-method   ${str(cpus, 'enable-method')}`);
  console.log(`#addr/#size     ${u32s(cpus.props['#address-cells']?.[0] ?? Buffer.from([0,0,0,1]))[0]}/${u32s(cpus.props['#size-cells']?.[0] ?? Buffer.from([0,0,0,0]))[0]}`);
  for (const c of cpus.children) {
    if (!c.name.startsWith('cpu')) continue;
    const reg = u32s(c.props.reg?.[0] ?? Buffer.alloc(0));
    console.log(`  ${c.name.padEnd(10)} reg=[${reg.map(hx).join(', ')}]  compat=${str(c, 'compatible')}  tb=${c.props['timebase-frequency'] ? hx(u32s(c.props['timebase-frequency'][0])[0]) : '-'}`);
  }
  const map = findChild(cpus, 'cpu-map');
  if (map) {
    console.log('  --- cpu-map ---');
    for (const cl of map.children) {
      const cores = cl.children.map((x) => `${x.name}->${u32s(x.props['cpu']?.[0] ?? Buffer.alloc(0)).map(hx).join(',')}`);
      console.log(`  ${cl.name}: ${cores.join('  ')}`);
    }
  }
  const pmu = findChild(cpus, 'pmu');
  if (pmu) console.log(`  pmu compat=${str(pmu, 'compatible')} interrupts=${hex(pmu.props.interrupts?.[0] ?? Buffer.alloc(0))}`);
}

console.log('');
console.log('=== interrupt controllers ===');
for (const n of collect(root, (x) => str(x, 'compatible')?.includes('gic'))) {
  console.log(`  ${pathOf(n)}`);
  console.log(`    compatible  ${str(n, 'compatible')}`);
  console.log(`    reg         ${regStr(n)}`);
  if (n.props['redistributor-stride']) console.log(`    redist-stride ${hx(u32s(n.props['redistributor-stride'][0])[0])}`);
  if (n.props['interrupts']) console.log(`    interrupts  ${hex(n.props['interrupts'][0])}`);
}

console.log('');
console.log('=== timer / pmu ===');
for (const n of collect(root, (x) => ['timer', 'arm,armv8-timer', 'arm,armv7-timer'].includes(x.name) || str(x, 'compatible')?.includes('armv8-timer'))) {
  console.log(`  ${pathOf(n)}  compat=${str(n, 'compatible')}`);
  console.log(`    interrupts ${hex(n.props.interrupts?.[0] ?? Buffer.alloc(0))}`);
  if (n.props['clock-frequency']) console.log(`    clock-frequency ${hx(u32s(n.props['clock-frequency'][0])[0])}`);
}

console.log('');
console.log('=== memory ===');
for (const n of collect(root, (x) => x.name === 'memory' || x.name.startsWith('memory@'))) {
  console.log(`  ${pathOf(n)}  device_type=${str(n, 'device_type')}  reg=${regStr(n)}`);
}
const reserved = findChild(root, 'reserved-memory');
if (reserved) {
  const kids = reserved.children.map((c) => c.name);
  console.log(`  reserved-memory: ${kids.length} children`);
  for (const c of reserved.children) {
    if (/splash|demura|shm|llm|ramoops/.test(c.name)) console.log(`    ${c.name} reg=${regStr(c)}`);
  }
}

console.log('');
console.log('=== soc ===');
const soc = findChild(root, 'soc');
if (soc) {
  console.log(`  compatible ${str(soc, 'compatible')}`);
  const rng = soc.props.ranges?.[0];
  if (rng) console.log(`  ranges     ${u32s(rng).map(hx).join(' ')}`);
  console.log(`  children   ${soc.children.length}`);
  for (const n of collect(soc, (x) => /ufshc|ufsphy|qup_uart|geni_se|usb@|ssusb/i.test(x.name))) {
    console.log(`    ${n.name}  reg=${regStr(n)}  status=${str(n, 'status') ?? '-'}`);
  }
}

console.log('');
console.log('=== chosen ===');
const chosen = findChild(root, 'chosen');
if (chosen) for (const k of Object.keys(chosen.props)) {
  const v = chosen.props[k][0];
  const printable = v.length > 0 && v.every((b) => b === 0 || (b >= 32 && b < 127));
  console.log(`  ${k} = ${printable ? JSON.stringify(v.toString('utf8').replace(/\0+$/, '')) : hex(v.slice(0, 32))}`);
}
console.log('');
console.log('=== psci ===');
const psci = findChild(root, 'psci');
if (psci) console.log(`  method=${str(psci, 'method')} compat=${str(psci, 'compatible')}`);
