import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const get = (n,p) => n?.props.find(x=>x.name===p)?.value;
const t = parseFdt(readFileSync(process.argv[2]));
const soc = t.root.children[0];
const frags = soc.children.filter(c => c.name.startsWith("fragment@"));
console.log(`fragment 数量: ${frags.length}`);
console.log("");
for (const f of frags) {
  const tp = get(f, "target-path");
  const tpStr = tp ? tp.toString().replace(/\0+$/,"") : null;
  const ov = f.children.find(c => c.name === "__overlay__");
  const kids = ov ? ov.children.map(c => c.name) : [];
  const props = ov ? ov.props.map(p => p.name) : [];
  console.log(`${f.name}  target=${tpStr ?? "(phandle)"}`);
  if (props.length) console.log(`    props: ${props.join(", ")}`);
  if (kids.length) console.log(`    kids : ${kids.slice(0,20).join(", ")}${kids.length>20?` ...(+${kids.length-20})`:""}`);
}
