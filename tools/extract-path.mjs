import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const t = parseFdt(readFileSync(process.argv[2]));
const root = t.root.children[0];
const at = (path) => {
  const parts = path.split("/").filter(Boolean);
  let n = root;
  for (const seg of parts) {
    const c = n.children.find(x => x.name === seg);
    if (!c) return null;
    n = c;
  }
  return n;
};
const show = (path) => {
  const n = at(path);
  if (!n) { console.log(`\n### ${path}  -> NOT FOUND`); return; }
  console.log(`\n### ${path}`);
  for (const pr of n.props) {
    if (pr.name === "phandle") continue;
    console.log(`    ${pr.name} = ${fmtVal(pr.value, 40)}`);
  }
  const kids = n.children.map(c => c.name);
  if (kids.length) console.log(`    -- children: ${kids.join(", ")}`);
};
show("/soc/qcom,qupv3_1_geni_se@ac0000/qcom,qup_uart@a9c000");
show("/soc/qcom,qupv3_1_geni_se@ac0000");
show("/soc/usb0");
show("/soc/usb@a600000");
