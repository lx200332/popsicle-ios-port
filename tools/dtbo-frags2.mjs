import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const get = (n,p) => n?.props.find(x=>x.name===p)?.value;
const t = parseFdt(readFileSync(process.argv[2]));
const soc = t.root.children[0];

// 建 phandle -> path 映射
const ph2path = new Map();
const walk = (n, path) => {
  const ph = get(n, "phandle") ?? get(n, "linux,phandle");
  if (ph && ph.length === 4) ph2path.set(ph.readUInt32BE(0), path);
  for (const c of n.children) walk(c, path + "/" + c.name);
};
walk(soc, "");

const frags = soc.children.filter(c => c.name.startsWith("fragment@"));
const start = Number(process.argv[3] || 0);
for (const f of frags.slice(start)) {
  const tp = get(f, "target");
  const tpStr = get(f, "target-path")?.toString().replace(/\0+$/,"");
  const ph = tp && tp.length === 4 ? tp.readUInt32BE(0) : null;
  const resolved = tpStr ?? (ph != null ? (ph2path.get(ph) ?? `phandle 0x${ph.toString(16)} ?`) : "?");
  const ov = f.children.find(c => c.name === "__overlay__");
  const kids = ov ? ov.children.map(c => c.name) : [];
  console.log(`${f.name}  -> ${resolved}`);
  if (kids.length) console.log(`    kids: ${kids.slice(0,16).join(", ")}${kids.length>16?` ...(+${kids.length-16})`:""}`);
}
