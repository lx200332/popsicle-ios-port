import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const buf = readFileSync(process.argv[2]);
const t = parseFdt(buf.subarray(0x1116c, 0x1116c + 201944));
const root = t.root.children[0];
const at = (path) => {
  const parts = path.split("/").filter(Boolean);
  let n = root;
  for (const seg of parts) { const c = n.children.find(x=>x.name===seg); if (!c) return null; n = c; }
  return n;
};
const show = (path) => {
  const n = at(path);
  if (!n) { console.log(`\n### ${path} -> NOT FOUND`); return; }
  console.log(`\n### ${path}  (props=${n.props.length} kids=${n.children.length})`);
  for (const p of n.props) console.log(`    ${p.name} = ${fmtVal(p.value, 32)}`);
  if (n.children.length) console.log(`    kids: ${n.children.map(c=>c.name).join(", ")}`);
};
show("/soc/ddr");
show("/soc");
show("/sw/smem");
