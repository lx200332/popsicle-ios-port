#!/usr/bin/env node
// 探查 dtbo overlay 的 fixup 结构
import { readFileSync } from 'node:fs';
import { parseFdt } from './fdt.mjs';

const t = parseFdt(readFileSync(process.argv[2] ?? '../extract/dtbo/dtbo_01_id0_rev0.dtb'));
console.log('root children:', t.root.children.map(c => c.name).join(', '));

for (const c of t.root.children) {
  if (['__fixups__', '__local_fixups__', '__symbols__'].includes(c.name)) {
    console.log(`\n[${c.name}] props=${c.props.length} children=${c.children.length}`);
    for (const p of c.props.slice(0, 8)) {
      console.log('   ', p.name, '=', p.value.toString('utf8').replace(/\0/g, ' | '));
    }
  }
}

const frags = t.root.children.filter(c => c.name.startsWith('fragment@'));
console.log('\nfragments:', frags.length);
if (frags[0]) {
  console.log('frag0 props:', frags[0].props.map(p => `${p.name}=[${p.value.toString('hex')}]`).join(' '));
  console.log('frag0 children:', frags[0].children.map(c => c.name).join(','));
}
