// Port make-sheng-adt.py to make-popsicle-adt.py with popsicle's real addresses.
import fs from 'node:fs';
import path from 'node:path';

const SRC = 'C:\\<WORKSPACE>\\unixnu-sheng-src\\scripts\\make-sheng-adt.py';
const DSTDIR = 'C:\\<WORKSPACE>\\popsicle-port\\UniXNU-popsicle';
const DST = path.join(DSTDIR, 'scripts', 'make-popsicle-adt.py');

for (const d of ['configs', 'scripts', 'drivers', 'kits', 'patches', 'package']) {
  fs.mkdirSync(path.join(DSTDIR, d), { recursive: true });
}

let s = fs.readFileSync(SRC, 'utf8');

const pairs = [
  ['"""Generate the minimal sheng hardware Apple-format DT for the first boot.',
   '"""Generate the minimal popsicle hardware Apple-format DT for the first boot.'],
  ['sheng SM8550 values', 'popsicle SM8850 values'],
  ['GICv3 distributor 0x17100000/0x10000, redistributor 0x17180000/0x200000',
   'GICv3 distributor 0x17000000/0x10000, redistributor 0x17080000/0x200000'],
  ['GIC_DIST = 0x17100000', 'GIC_DIST = 0x17000000'],
  ['GIC_REDIST = 0x17180000', 'GIC_REDIST = 0x17080000'],
  ['build/sheng/boot/kernel.macho.adt', 'build/popsicle/boot/kernel.macho.adt'],
  ["b'xiaomi,sheng\\0qcom,sm8550\\0'", "b'xiaomi,popsicle\\0qcom,canoe\\0'"],
  ["b'sheng-first-boot\\0'", "b'popsicle-first-boot\\0'"],
  ["b'arm,cortex-a715\\0'", "b'arm,armv8\\0'"],
  ["b'qcom,sm8550-ufshc\\0'", "b'qcom,canoe-ufshc\\0'"],
  ["b'Xiaomi Pad 6S Pro 12.4\\0'", "b'Xiaomi 17 Pro Max\\0'"],
  ["b'xiaomi,sheng\\0'", "b'xiaomi,popsicle\\0'"],
  ['SM8550 numbers its cores 0x000..0x700 in Affinity1 (sm8550.dtsi)',
   'SM8850 numbers its cluster-0 cores 0x000..0x500 (canoe.dtsi); the two cluster-1 cores sit at 0x10000/0x10100 and are left off here'],
  ["the panel geometry and button set are sheng's", "the panel geometry and button set are popsicle's"],
  ['sheng-data-persist', 'popsicle-data-persist'],
  ["print(f'sheng ADT:", "print(f'popsicle ADT:"],
  ["sheng's", "popsicle's"],
  ["sheng\\'s", "popsicle\\'s"],
  ['# Root compatible strings of the compiled board DT (xiaomi,sheng / qcom,sm8550).',
   '# Root compatible strings of the compiled board DT (xiaomi,popsicle / qcom,canoe).'],
  ["# No 'menu' entry: sheng has no Home button.", "# No 'menu' entry: popsicle has no Home button."],
];
for (const [from, to] of pairs) s = s.split(from).join(to);

fs.writeFileSync(DST, s);
console.log(`wrote ${DST} (${s.length} bytes)`);
const left = s.split('\n').map((l, i) => [i + 1, l]).filter(([, l]) => /sheng|sm8550/i.test(l));
console.log(`remaining sheng/sm8550 lines: ${left.length}`);
for (const [n, l] of left) console.log(`  ${n}: ${l.trim()}`);
