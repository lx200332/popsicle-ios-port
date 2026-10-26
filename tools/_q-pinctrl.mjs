import { readFileSync } from 'node:fs';
import { parseFdt } from 'file:///C:/dsh-%E5%AF%B9%E8%AF%9D/popsicle-port/tools/fdt.mjs';

const buf = readFileSync(process.argv[2]);
const t = parseFdt(buf);
const real = t.root ?? (t.children && t.children[0]);

function walk(n, path) {
  if (n.name && n.name.startsWith('pinctrl@')) {
    console.log('NODE ' + path + '/' + n.name);
    for (const p of n.props) {
      if (p.name === 'reg') console.log('  reg: ' + p.value.map(v => '0x' + v.toString(16)).join(' '));
      if (p.name === 'compatible') console.log('  compatible: ' + p.value.map(v => String(v)).join(','));
    }
  }
  for (const c of n.children ?? []) walk(c, path + '/' + n.name);
}
walk(real, '');
