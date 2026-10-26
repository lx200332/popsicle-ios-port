import { readFileSync, writeFileSync } from "node:fs";
const buf = readFileSync(process.argv[2]);
const min = Number(process.argv[3] || 6);
const out = [];
let cur = [];
for (let i = 0; i < buf.length; i++) {
  const c = buf[i];
  if (c >= 0x20 && c < 0x7f) cur.push(c);
  else {
    if (cur.length >= min) out.push({ off: i - cur.length, s: Buffer.from(cur).toString("latin1") });
    cur = [];
  }
}
if (cur.length >= min) out.push({ off: buf.length - cur.length, s: Buffer.from(cur).toString("latin1") });
console.log(`strings(>=${min}) 总数: ${out.length}`);
writeFileSync(process.argv[4], out.map(o => `0x${o.off.toString(16).padStart(6,"0")}  ${o.s}`).join("\n"));
console.log(`已写入 ${process.argv[4]}`);
