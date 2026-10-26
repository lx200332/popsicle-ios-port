import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const get = (n,p) => n?.props.find(x=>x.name===p)?.value;
const t = parseFdt(readFileSync(process.argv[2]));
const soc = t.root.children[0];
const rm = soc.children.find(c => c.name === "reserved-memory");
console.log(`reserved-memory 子节点: ${rm.children.length}\n`);
const rows = [];
for (const c of rm.children) {
  const reg = get(c, "reg");
  const size = get(c, "size");
  const st = get(c, "status")?.toString().replace(/\0+$/,"") ?? "";
  const noMap = c.props.some(p => p.name === "no-map");
  const reusable = c.props.some(p => p.name === "reusable");
  let base = null, sz = null;
  if (reg && reg.length >= 8) {
    base = reg.readUInt32BE(0) * 4294967296 + reg.readUInt32BE(4);
    sz = reg.readUInt32BE(8) * 4294967296 + reg.readUInt32BE(12);
  }
  rows.push({ name: c.name, base, sz, st, noMap, reusable });
}
rows.sort((a,b) => (a.base ?? 0) - (b.base ?? 0));
const hex = n => n == null ? "-" : "0x" + n.toString(16).padStart(10,"0");
const mib = n => n == null ? "" : (n/1048576).toFixed(1) + " MiB";
console.log("base          size          size        name                                  flags");
for (const r of rows) {
  const flags = [r.noMap?"no-map":"", r.reusable?"reusable":"", r.st?`status=${r.st}`:""].filter(Boolean).join(",");
  console.log(`${hex(r.base)}  ${hex(r.sz)}  ${mib(r.sz).padStart(10)}  ${r.name.padEnd(36)}  ${flags}`);
}
