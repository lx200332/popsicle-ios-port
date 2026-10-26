import { readFileSync } from "node:fs";
import { parseFdt } from "./fdt.mjs";
const t = parseFdt(readFileSync(process.argv[2]));
const soc = t.root.children[0];
const rm = soc.children.find(c => c.name === "reserved-memory");
console.log("reserved-memory #address-cells =", rm.props.find(p=>p.name==="#address-cells")?.value.readUInt32BE(0));
console.log("reserved-memory #size-cells    =", rm.props.find(p=>p.name==="#size-cells")?.value.readUInt32BE(0));
console.log("");
for (const nm of ["lcp_dare_region","llcc_lpi_region@ff800000","oem_vm_region@f8800000","trust_ui_vm_region@f3800000","hypervisor_extended_heap_region@0xc3dd0000","gunyah_hyp_region@80000000"]) {
  const c = rm.children.find(x => x.name === nm);
  if (!c) { console.log(`${nm}: not found`); continue; }
  const reg = c.props.find(p => p.name === "reg")?.value;
  console.log(`${nm}`);
  console.log(`   reg bytes(${reg.length}) = ${reg.toString("hex")}`);
  const cells = [];
  for (let i = 0; i + 4 <= reg.length; i += 4) cells.push("0x" + reg.readUInt32BE(i).toString(16));
  console.log(`   cells = ${cells.join(" ")}`);
}
