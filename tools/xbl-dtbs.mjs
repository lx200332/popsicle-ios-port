import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const buf = readFileSync(process.argv[2]);
const get = (n,p) => n?.props.find(x=>x.name===p)?.value;
for (const [off, size, tag] of [[0x31c, 67840, "pre-ddr"], [0x1116c, 201944, "post-ddr"]]) {
  const t = parseFdt(buf.subarray(off, off + size));
  const r = t.root;
  const soc = r.children[0] ?? r;
  console.log(`\n########## ${tag}  @0x${off.toString(16)} size=${size} ##########`);
  console.log(`root.name="${r.name}" kids=${r.children.length}`);
  console.log(`model      = ${fmtVal(get(soc,"model") ?? get(r,"model"))}`);
  console.log(`compatible = ${fmtVal(get(soc,"compatible") ?? get(r,"compatible"))}`);
  console.log(`msm-id     = ${fmtVal(get(soc,"qcom,msm-id") ?? get(r,"qcom,msm-id"))}`);
  console.log(`board-id   = ${fmtVal(get(soc,"qcom,board-id") ?? get(r,"qcom,board-id"))}`);
  console.log(`children   = ${r.children.map(c=>c.name).join(", ")}`);
  // 找 memory / ddr 相关
  const hits = [];
  const walk = (n, path) => {
    const p = path + "/" + n.name;
    if (/memory|ddr|mem|ram/i.test(n.name)) hits.push(p);
    for (const c of n.children) walk(c, p);
  };
  walk(r, "");
  if (hits.length) console.log(`mem/ddr 节点:\n  ${hits.slice(0,40).join("\n  ")}`);
}
