
const t = require('./fdt.mjs').parseFdt(process.argv[2]);
const real = t.root;
function walk(n, path) {
  if (n.name === 'pinctrl@f000000' || n.name === 'pinctrl@f100000' || n.name.startsWith('pinctrl@')) {
    console.log('NODE', path + '/' + n.name);
    for (const p of n.props) {
      if (p.name === 'reg') console.log('  reg:', p.value.map(v => '0x' + v.toString(16)).join(' '));
      if (p.name === 'compatible') console.log('  compatible:', p.value.map(v => v.toString()).join(','));
    }
  }
  for (const c of n.children) walk(c, path + '/' + n.name);
}
walk(real, '');
