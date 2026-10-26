import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const t = parseFdt(readFileSync(process.argv[2]));
const soc = t.root.children[0];
const found = [];
const walk = (n, path) => {
  const p = path + "/" + n.name;
  if (/qup_uart|usb0|usb@|dwc3|usb_qmp|typec|pmic_glink|smem|glink/i.test(n.name)) found.push({ n, p });
  for (const c of n.children) walk(c, p);
};
walk(soc, "");
for (const { n, p } of found) {
  console.log(`\n### ${p}`);
  for (const pr of n.props) {
    if (pr.name === "phandle") continue;
    console.log(`    ${pr.name} = ${fmtVal(pr.value, 40)}`);
  }
}
