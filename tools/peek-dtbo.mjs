import { readFileSync } from "node:fs";
import { findFdtOffsets, parseFdt, fmtVal } from "./fdt.mjs";
const get = (n,p) => n?.props.find(x=>x.name===p)?.value;
for (const f of process.argv.slice(2)) {
  const buf = readFileSync(f);
  const hits = findFdtOffsets(buf);
  console.log(`\n=== ${f}  size=${buf.length}  FDTs=${hits.length} ===`);
  for (const h of hits) {
    const t = parseFdt(buf.subarray(h.offset, h.offset + h.size));
    const r = t.root;
    const soc = r.children[0] || r;
    console.log(`  @0x${h.offset.toString(16)} size=${h.size}`);
    console.log(`     root.name="${r.name}" rootKids=${r.children.length}`);
    console.log(`     model=${fmtVal(get(soc,"model") || get(r,"model"))}`);
    console.log(`     compatible=${fmtVal(get(soc,"compatible") || get(r,"compatible"))}`);
    console.log(`     msm-id=${fmtVal(get(soc,"qcom,msm-id") || get(r,"qcom,msm-id"))}`);
    console.log(`     board-id=${fmtVal(get(soc,"qcom,board-id") || get(r,"qcom,board-id"))}`);
    console.log(`     kids=${soc.children.map(c=>c.name).slice(0,12).join(",")}`);
  }
}
