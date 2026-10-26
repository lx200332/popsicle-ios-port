#!/usr/bin/env node
// 验证合并后的 popsicle DTB
import { readFileSync } from 'node:fs';
import { parseFdt } from './fdt.mjs';

const t = parseFdt(readFileSync(process.argv[2]));
const root = t.root.children[0];
const child = (n, s) => n.children.find(c => c.name === s);
const getProp = (n, s) => n.props.find(p => p.name === s);
const str = (n, s) => { const p = getProp(n, s); return p ? p.value.toString('utf8').replace(/\0+$/, '') : null; };

console.log('=== identity ===');
console.log('model        :', str(root, 'model'));
console.log('compatible   :', str(root, 'compatible'));
console.log('qcom,board-id:', getProp(root, 'qcom,board-id') ? '<' + [...Array(getProp(root, 'qcom,board-id').value.length / 4)].map((_, i) => '0x' + getProp(root, 'qcom,board-id').value.readUInt32BE(i * 4).toString(16)).join(' ') + '>' : null);
console.log('qcom,msm-id  :', getProp(root, 'qcom,msm-id') ? '<' + [...Array(getProp(root, 'qcom,msm-id').value.length / 4)].map((_, i) => '0x' + getProp(root, 'qcom,msm-id').value.readUInt32BE(i * 4).toString(16)).join(' ') + '>' : null);

console.log('\n=== serial console ===');
const chosen = child(root, 'chosen');
if (chosen) {
  console.log('stdout-path:', str(chosen, 'stdout-path'));
  const ba = str(chosen, 'bootargs');
  console.log('bootargs   :', ba ? ba.slice(0, 200) : null);
}

console.log('\n=== popsicle-specific (from overlay) ===');
const soc = child(root, 'soc');
console.log('soc children:', soc ? soc.children.length : 'NO SOC');

const markers = [
  ['nt37801 panel', n => n.name.includes('nt37801')],
  ['p2_42_02_0a panel', n => n.name.includes('p2_42_02_0a')],
  ['synaptics_tcm', n => n.name.includes('synaptics_tcm')],
  ['goodix_gt9615v', n => n.name.includes('goodix_gt9615v')],
  ['xiaomi_fingerprint', n => n.name.includes('xiaomi_fingerprint')],
  ['mi_t1_chip', n => n.name.includes('mi_t1_chip')],
  ['cnss-peach (wlan)', n => n.name.includes('cnss-peach')],
  ['nq@28 (nfc)', n => n.name === 'nq@28'],
  ['gpio_keys', n => n.name === 'gpio_keys'],
  ['dsi_panel_pwr_supply_p2', n => n.name.includes('dsi_panel_pwr_supply_p2')],
];
const hits = new Map(markers.map(m => [m[0], 0]));
const walk = n => { for (const m of markers) if (m[1](n)) hits.set(m[0], hits.get(m[0]) + 1); n.children.forEach(walk); };
walk(root);
for (const [k, v] of hits) console.log(`  ${v > 0 ? 'OK ' : 'MISS'} ${k}: ${v}`);

console.log('\n=== UFS / USB enabled ===');
for (const name of ['ufshc@1d84000', 'ufsphy_mem@1d87000', 'ssusb@a600000', 'qcom,qup_uart@a9c000']) {
  let found = null;
  const find = n => { if (!found && n.name === name) found = n; n.children.forEach(find); };
  find(root);
  if (found) console.log(`  OK  ${name}  status=${str(found, 'status') ?? '(none)'}`);
  else console.log(`  MISS ${name}`);
}

console.log('\n=== reserved-memory (popsicle additions) ===');
const rm = child(root, 'reserved-memory');
if (rm) {
  for (const nm of ['splash_region', 'demura_region_0', 'demura_region_1', 'shm@fc200000', 'llm_mem', 'cnss_wlan_region']) {
    const n = rm.children.find(c => c.name === nm);
    if (n) {
      const reg = getProp(n, 'reg');
      const val = reg ? '<' + [...Array(reg.value.length / 4)].map((_, i) => '0x' + reg.value.readUInt32BE(i * 4).toString(16)).join(' ') + '>' : '(no reg)';
      console.log(`  OK  ${nm} ${val}`);
    } else console.log(`  --  ${nm} (absent)`);
  }
  console.log('  total reserved-memory children:', rm.children.length);
}

console.log('\n=== phandle integrity ===');
let maxPh = 0, nodesWithPh = 0, dangling = 0;
const phSet = new Set();
const all = [];
const collect = n => { all.push(n); const p = getProp(n, 'phandle'); if (p) { const v = p.value.readUInt32BE(0); phSet.add(v); nodesWithPh++; if (v > maxPh) maxPh = v; } n.children.forEach(collect); };
collect(root);
// 检查所有 cell 属性里像 phandle 的引用是否有效（仅检查已知 phandle 属性名）
const PH_PROPS = ['interrupt-parent', 'phandle', 'clocks', 'resets', 'power-domains', 'iommus', 'mboxes', 'gpios', 'cs-gpios', 'pinctrl-0', 'pinctrl-1', 'phys', 'vdd-supply', 'vdda-supply', 'io-supply', 'qcom,glink-channels'];
const check = n => {
  for (const p of n.props) {
    if (p.name === 'phandle' || p.name === 'linux,phandle') continue;
    if (p.value.length % 4 !== 0 || p.value.length === 0) continue;
    const nCells = p.value.length / 4;
    if (nCells > 8) continue; // 大数组多为 reg/字符串，跳过
    for (let i = 0; i < nCells; i++) {
      const v = p.value.readUInt32BE(i * 4);
      if (v > 0 && v <= maxPh && !phSet.has(v) && PH_PROPS.includes(p.name)) dangling++;
    }
  }
  n.children.forEach(check);
};
check(root);
console.log(`  nodes with phandle: ${nodesWithPh}, max phandle: 0x${maxPh.toString(16)}`);
console.log(`  suspicious refs in phandle props: ${dangling}`);

console.log('\n=== size ===');
console.log('  dtb bytes:', t.header.totalsize);
