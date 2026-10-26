import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const t = parseFdt(readFileSync(process.argv[2]));
const root = t.root.children[0];
const out = [];
const walk = (n, path) => {
  const p = path + "/" + n.name;
  const comp = n.props.find(x => x.name === "compatible");
  const cs = comp ? fmtVal(comp.value) : "";
  if (/dwc3|dwc_usb|qcom,usb|snps|hs-phy|ss-phy|qmp|usb/i.test(cs) || /usb/i.test(n.name)) {
    out.push({ p, cs, reg: fmtVal(n.props.find(x=>x.name==="reg")?.value ?? Buffer.alloc(0), 24), st: fmtVal(n.props.find(x=>x.name==="status")?.value ?? Buffer.alloc(0)) });
  }
  for (const c of n.children) walk(c, p);
};
walk(root, "");
for (const o of out) {
  console.log(`${o.p}`);
  console.log(`    compatible = ${o.cs}`);
  if (o.reg !== "") console.log(`    reg = ${o.reg}`);
  if (o.st !== "") console.log(`    status = ${o.st}`);
}
