import { readFileSync } from "node:fs";
import { parseFdt } from "./fdt.mjs";
const t = parseFdt(readFileSync(process.argv[2]));
const soc = t.root.children[0];
const fx = soc.children.find(c => c.name === "__fixups__");
// 反转：fragment 名 -> 符号名
const frag2sym = new Map();
for (const p of fx.props) {
  const val = p.value.toString().replace(/\0+$/,"");
  for (const part of val.split("\0")) {
    if (!part) continue;
    const seg = part.split(":");
    const path = seg[0];
    const prop = seg[1];
    if (prop !== "target") continue;
    const m = path.match(/fragment@(\d+)/);
    if (m) frag2sym.set("fragment@" + m[1], p.name);
  }
}
const frags = soc.children.filter(c => c.name.startsWith("fragment@"));
console.log(`fragment=${frags.length}  已解析 target 符号=${frag2sym.size}\n`);
for (const f of frags) {
  const sym = frag2sym.get(f.name);
  const ov = f.children.find(c => c.name === "__overlay__");
  const kids = ov ? ov.children.map(c=>c.name) : [];
  const st = ov?.props.find(p=>p.name==="status")?.value.toString().replace(/\0+$/,"");
  if (!kids.length && !st) continue;
  console.log(`${f.name.padEnd(13)} -> ${sym ?? "(target-path)"}${st?`  [${st}]`:""}`);
  if (kids.length) console.log(`              ${kids.slice(0,12).join(", ")}${kids.length>12?` ...(+${kids.length-12})`:""}`);
}
