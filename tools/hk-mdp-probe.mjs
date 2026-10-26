import { readFileSync } from 'fs';
import { parseFdt } from './fdt.mjs';
const t = parseFdt(readFileSync('C:/dsh-tools/cache/popsicle.dtb'));
const real = t.root;
function cells(v) { const r = []; if (!v) return r; for (let i=0;i+3<v.length;i+=4) r.push(v.readUInt32BE(i)); return r; }
function walk(node) {
  if (!node) return;
  const name = node.name || '';
  if (name.includes('mdss_mdp') || name.includes('mdp@')) {
    let ssp = null, reg = null;
    for (const p of node.props) { if (p.name==='qcom,sde-sspp-off') ssp=p.value; if (p.name==='reg') reg=p.value; }
    console.log('NODE', name);
    if (reg) console.log('  reg =', cells(reg).map(x=>'0x'+x.toString(16)).join(' '));
    if (ssp) console.log('  sde-sspp-off =', cells(ssp).map(x=>'0x'+x.toString(16)).join(' '));
  }
  if (name.includes('splash')) {
    let reg = null;
    for (const p of node.props) if (p.name==='reg') reg=p.value;
    if (reg) console.log('SPLASH', name, cells(reg).map(x=>'0x'+x.toString(16)).join(' '));
  }
  for (const c of (node.children||[])) walk(c);
}
walk(real);
