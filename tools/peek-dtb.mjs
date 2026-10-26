import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const buf = readFileSync(process.argv[2]);
const t = parseFdt(buf);
const walk = (n, d) => {
  if (d > 2) return;
  console.log(`${"  ".repeat(d)}[${n.name || "/"}] props=${n.props.length} kids=${n.children.length}`);
  for (const p of n.props.slice(0, 12)) console.log(`${"  ".repeat(d)}   . ${p.name} = ${fmtVal(p.value)}`);
  for (const c of n.children.slice(0, 40)) walk(c, d + 1);
};
walk(t.root, 0);
