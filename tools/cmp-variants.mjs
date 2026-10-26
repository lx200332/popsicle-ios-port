import { readFileSync } from "node:fs";
import { parseFdt, fmtVal } from "./fdt.mjs";
const dir = process.argv[2];
const get = (n, p) => n?.props.find(x => x.name === p)?.value;
const pairs = v => { const o=[]; for (let i=0;i+8<=v.length;i+=8) o.push([v.readUInt32BE(i), v.readUInt32BE(i+4)]); return o; };

console.log("########## 12 个变体 ##########");
for (let i = 0; i < 12; i++) {
  const f = `${dir}/variant_${String(i).padStart(2,"0")}.dtb`;
  const t = parseFdt(readFileSync(f));
  const soc = t.root.children[0];
  const mem = soc.children.find(c => c.name === "memory");
  const mr = get(mem, "reg");
  const mp = mr ? pairs(mr).map(([a,s]) => `0x${a.toString(16)}+0x${s.toString(16)}`).join(" ") : "-";
  console.log(`v${String(i).padStart(2,"0")}  ${fmtVal(get(soc,"model"))}`);
  console.log(`     msm-id=${fmtVal(get(soc,"qcom,msm-id"))}  board-id=${fmtVal(get(soc,"qcom,board-id"))}  memory=${mp}`);
}
