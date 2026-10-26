import { readFileSync, writeFileSync } from "node:fs";
const buf = readFileSync(process.argv[2]);

// --- 1. 扫描 FDT magic ---
const MAGIC = 0xd00dfeed;
console.log("=== xbl_config 内的 FDT ===");
for (let i = 0; i + 40 <= buf.length; i += 4) {
  if (buf.readUInt32BE(i) !== MAGIC) continue;
  const total = buf.readUInt32BE(i + 4);
  if (total < 40 || i + total > buf.length) continue;
  console.log(`  FDT @ 0x${i.toString(16)}  size=${total}`);
  i += total - 4;
}

// --- 2. 解析 memorymap 节点树 ---
const START = 0x295c0;
let off = START;
const root = { name: "", props: [], children: [] };
const stack = [root];
let guard = 0;
while (off + 4 <= buf.length && guard++ < 200000) {
  const type = buf.readUInt32BE(off);
  if (type === 1) {
    off += 4;
    let e = off; while (e < buf.length && buf[e] !== 0) e++;
    const name = buf.subarray(off, e).toString("latin1");
    const total = e - off + 1;
    off += (total + 3) & ~3;
    const node = { name, props: [], children: [] };
    stack[stack.length - 1].children.push(node);
    stack.push(node);
  } else if (type === 2) {
    off += 4;
    if (stack.length > 1) stack.pop();
    else break;
  } else if (type === 3) {
    off += 4;
    const len = buf.readUInt32BE(off); off += 4;
    const nameoff = buf.readUInt32BE(off); off += 4;
    const val = Buffer.from(buf.subarray(off, off + len));
    off += (len + 3) & ~3;
    stack[stack.length - 1].props.push({ nameoff, val });
  } else break;
}
console.log(`\n解析结束 @ 0x${off.toString(16)}；根子节点数 = ${root.children.length}`);
const mm = root.children[0];
if (!mm) { console.log("no memorymap node"); process.exit(0); }
console.log(`节点 "${mm.name}" 子节点数 = ${mm.children.length}\n`);

const printable = (b) => {
  for (const c of b) if (c !== 0 && (c < 0x20 || c > 0x7e)) return false;
  return b.length > 0;
};
const rows = [];
for (const n of mm.children) {
  let base = null, size = null;
  const tags = [];
  for (const p of n.props) {
    if (p.val.length === 16) {
      base = Number(p.val.readBigUInt64BE(0));
      size = Number(p.val.readBigUInt64BE(8));
    } else if (printable(p.val)) {
      const s = p.val.toString("latin1").replace(/\0+$/, "");
      if (s) tags.push(s);
    }
  }
  rows.push({ name: n.name, base, size, tags, nprops: n.props.length });
}
rows.sort((a, b) => (a.base ?? 0) - (b.base ?? 0));
console.log("base           size         MiB      node                          tags");
for (const r of rows) {
  const b = r.base == null ? "-" : "0x" + r.base.toString(16).padStart(10, "0");
  const s = r.size == null ? "-" : "0x" + r.size.toString(16).padStart(10, "0");
  const mib = r.size == null ? "" : (r.size / 1048576).toFixed(2);
  console.log(`${b}  ${s}  ${mib.padStart(8)}  ${r.name.padEnd(28)}  ${r.tags.join(",")}`);
}
const top = rows.filter(r => r.base != null && r.size != null).map(r => r.base + r.size);
console.log(`\n最高保留区末端 = 0x${Math.max(...top).toString(16)}  (${(Math.max(...top)/1073741824).toFixed(2)} GiB)`);
writeFileSync(process.argv[3], JSON.stringify(rows, null, 2));
console.log(`已写入 ${process.argv[3]}`);
