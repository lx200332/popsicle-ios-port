import { readFileSync, writeFileSync } from "node:fs";
const buf = readFileSync(process.argv[2]);
const start = parseInt(process.argv[3], 16);
const len = parseInt(process.argv[4], 16);
const seg = buf.subarray(start, start + len);
writeFileSync(process.argv[5], seg);
console.log(`dumped 0x${start.toString(16)} .. 0x${(start+len).toString(16)} -> ${process.argv[5]} (${seg.length} bytes)\n`);
// 逐行 hex + ascii
for (let i = 0; i < seg.length; i += 16) {
  const row = seg.subarray(i, i + 16);
  const hex = [...row].map(b => b.toString(16).padStart(2,"0")).join(" ");
  const asc = [...row].map(b => (b >= 32 && b < 127) ? String.fromCharCode(b) : ".").join("");
  console.log(`${(start+i).toString(16).padStart(6,"0")}  ${hex.padEnd(47)}  ${asc}`);
}
