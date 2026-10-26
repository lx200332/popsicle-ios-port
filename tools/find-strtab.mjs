import { readFileSync } from "node:fs";
const buf = readFileSync(process.argv[2]);
const cstr = (o) => { let e=o; while (e<buf.length && buf[e]!==0) e++; return buf.subarray(o,e).toString("latin1"); };
// 测假设：字符串表基址 S，使 S+0xc4c == "memory"
for (const S of [0, 0x200, 0x28a00, 0x289d0, 0x28a60]) {
  console.log(`S=0x${S.toString(16)}: S+0xc4c="${cstr(S+0xc4c)}"  S+0xbe="${cstr(S+0xbe)}"`);
}
console.log("\n=== CFGL 头 (0x1F0..0x2A0) ===");
for (let i = 0x1f0; i < 0x2a0; i += 16) {
  const row = buf.subarray(i, i+16);
  const hex = [...row].map(b=>b.toString(16).padStart(2,"0")).join(" ");
  const asc = [...row].map(b=>(b>=32&&b<127)?String.fromCharCode(b):".").join("");
  console.log(`${i.toString(16).padStart(5,"0")}  ${hex}  ${asc}`);
}
console.log("\n=== 直接找独立 \"memory\\0\" 字符串 ===");
for (let i = 0; i + 7 <= buf.length; i++) {
  if (buf[i]===0x6d && buf.toString("latin1", i, i+6)==="memory" && buf[i+6]===0) {
    console.log(`  offset 0x${i.toString(16)}`);
  }
}
