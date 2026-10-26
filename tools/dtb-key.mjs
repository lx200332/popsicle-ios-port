import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const t = parseFdt(readFileSync(process.argv[2]));
const soc = t.root.children[0];
const get = (n, p) => n?.props.find(x => x.name === p)?.value;
const asPairs = v => {
  const out = [];
  for (let i = 0; i + 8 <= v.length; i += 8) out.push([v.readUInt32BE(i), v.readUInt32BE(i + 4)]);
  return out;
};
console.log("model      :", fmtVal(get(soc, "model")));
console.log("compatible :", fmtVal(get(soc, "compatible")));
console.log("msm-id     :", fmtVal(get(soc, "qcom,msm-id")));
console.log("board-id   :", fmtVal(get(soc, "qcom,board-id")));
console.log("pmic-id    :", fmtVal(get(soc, "qcom,pmic-id")));

const mem = soc.children.find(c => c.name === "memory");
const reg = get(mem, "reg");
console.log("\n=== memory/reg (addr,size pairs) ===");
for (const [a, s] of asPairs(reg)) {
  console.log(`  base=0x${a.toString(16).padStart(8,"0")}  size=0x${s.toString(16).padStart(8,"0")}  (${(s/1024/1024/1024).toFixed(2)} GiB)`);
}

const chosen = soc.children.find(c => c.name === "chosen");
console.log("\n=== chosen/bootargs ===");
console.log(get(chosen, "bootargs")?.toString() || "(none)");
console.log("\n=== chosen/stdout-path ===");
console.log(fmtVal(get(chosen, "stdout-path") || Buffer.alloc(0)));

const fw = soc.children.find(c => c.name === "firmware");
console.log("\n=== firmware children ===");
for (const c of fw?.children || []) {
  const compat = get(c, "compatible");
  console.log(`  ${c.name}  compatible=${compat ? fmtVal(compat) : "-"}`);
}
