import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const get = (n,p) => n?.props.find(x=>x.name===p)?.value;
const t = parseFdt(readFileSync(process.argv[2]));
const soc = t.root.children[0];

// 全树查找名字含关键字的节点
const targets = [];
const walk = (n, path) => {
  const p = path + "/" + n.name;
  if (/qup_uart|geni_se@ac0000|usb@|dwc3|ufshc|sdhc|qcom,sde|mdss|disp_cc|gpu@|adreno|qseecom|crypto|rng/i.test(n.name)) targets.push({ n, p });
  for (const c of n.children) walk(c, p);
};
walk(soc, "");

for (const { n, p } of targets.slice(0, 40)) {
  console.log(`\n### ${p}`);
  for (const pr of n.props) {
    if (pr.name === "phandle") continue;
    const v = fmtVal(pr.value, 64);
    console.log(`    ${pr.name} = ${v}`);
  }
}
