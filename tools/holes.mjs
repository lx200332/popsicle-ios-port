import { readFileSync } from "node:fs";
import { parseFdt } from "./fdt.mjs";
const t = parseFdt(readFileSync(process.argv[2]));
const soc = t.root.children[0];
const rm = soc.children.find(c => c.name === "reserved-memory");
const fixed = [];
for (const c of rm.children) {
  const reg = c.props.find(p => p.name === "reg")?.value;
  if (!reg || reg.length < 16) continue;
  const base = reg.readUInt32BE(0) * 4294967296 + reg.readUInt32BE(4);
  const sz   = reg.readUInt32BE(8) * 4294967296 + reg.readUInt32BE(12);
  if (sz === 0) continue;
  fixed.push({ name: c.name, base, end: base + sz, sz });
}
fixed.sort((a,b) => a.base - b.base);

const H4G = 0x100000000;
const inLow = fixed.filter(r => r.base < H4G);
console.log("=== 4 GiB 以下的固定保留区 ===");
for (const r of inLow) {
  console.log(`  ${("0x"+r.base.toString(16)).padEnd(12)} .. ${("0x"+r.end.toString(16)).padEnd(12)} ${(r.sz/1048576).toFixed(1).padStart(7)} MiB  ${r.name}`);
}

console.log("\n=== 4 GiB 以下的空洞 (>= 4 MiB) ===");
const DRAM_BASE = 0x80000000;
let cursor = DRAM_BASE;
const holes = [];
for (const r of inLow) {
  if (r.base > cursor) {
    const gap = r.base - cursor;
    if (gap >= 4 * 1048576) holes.push({ start: cursor, end: r.base, sz: gap });
  }
  cursor = Math.max(cursor, r.end);
}
if (H4G > cursor) {
  const gap = H4G - cursor;
  if (gap >= 4 * 1048576) holes.push({ start: cursor, end: H4G, sz: gap });
}
for (const h of holes) {
  console.log(`  ${("0x"+h.start.toString(16)).padEnd(12)} .. ${("0x"+h.end.toString(16)).padEnd(12)} ${(h.sz/1048576).toFixed(1).padStart(7)} MiB`);
}
