import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const t = parseFdt(readFileSync(process.argv[2]));
const soc = t.root.children[0];
const fx = soc.children.find(c => c.name === "__fixups__");
const frag2sym = new Map();
for (const p of fx.props) {
  for (const part of p.value.toString().replace(/\0+$/,"").split("\0")) {
    if (!part) continue;
    const [path, prop] = part.split(":");
    if (prop !== "target") continue;
    const m = path.match(/fragment@(\d+)/);
    if (m) frag2sym.set("fragment@" + m[1], p.name);
  }
}
const frags = soc.children.filter(c => c.name.startsWith("fragment@"));
console.log("=== overlay 向 reserved-memory 追加的区域 ===");
for (const f of frags) {
  const sym = frag2sym.get(f.name);
  if (!/reserved_memory|reserved-memory/.test(sym ?? "")) continue;
  const ov = f.children.find(c => c.name === "__overlay__");
  if (!ov) continue;
  for (const c of ov.children) {
    const reg = c.props.find(p => p.name === "reg")?.value;
    let info = "(no reg)";
    if (reg && reg.length >= 16) {
      const base = reg.readUInt32BE(0) * 4294967296 + reg.readUInt32BE(4);
      const sz   = reg.readUInt32BE(8) * 4294967296 + reg.readUInt32BE(12);
      info = `0x${base.toString(16).padStart(10,"0")} .. 0x${(base+sz).toString(16).padStart(10,"0")}  ${(sz/1048576).toFixed(1)} MiB`;
    } else if (reg) {
      const cells = []; for (let i=0;i+4<=reg.length;i+=4) cells.push("0x"+reg.readUInt32BE(i).toString(16));
      info = `cells: ${cells.join(" ")}`;
    }
    const noMap = c.props.some(p=>p.name==="no-map");
    console.log(`  ${f.name} -> ${sym}`);
    console.log(`      ${c.name.padEnd(30)} ${info}${noMap?"  [no-map]":""}`);
  }
}
