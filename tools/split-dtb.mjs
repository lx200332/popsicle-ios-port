import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { join } from "node:path";
import { findFdtOffsets, parseFdt, fmtVal } from "./fdt.mjs";

const buf = readFileSync(process.argv[2]);
const outdir = process.argv[3];
mkdirSync(outdir, { recursive: true });
const hits = findFdtOffsets(buf);

const summary = [];
for (const h of hits) {
  const slice = buf.subarray(h.offset, h.offset + h.size);
  const t = parseFdt(slice);
  const props = Object.fromEntries(t.root.props.map(p => [p.name, fmtVal(p.value)]));
  const name = `variant_${String(summary.length).padStart(2, "0")}`;
  writeFileSync(join(outdir, name + ".dtb"), slice);
  // 只看根节点直接子节点的名字（用于识别机型差异）
  const kids = t.root.children.map(c => c.name);
  summary.push({
    name,
    offset: "0x" + h.offset.toString(16),
    size: h.size,
    model: props["model"],
    compatible: props["compatible"],
    qcom_board_id: props["qcom,board-id"],
    qcom_msm_id: props["qcom,msm-id"],
    qcom_pmic_id: props["qcom,pmic-id"],
    qcom_model: props["qcom,model"],
    rootChildren: kids.length,
  });
}
console.log(JSON.stringify(summary, null, 2));
