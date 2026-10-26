import { readFileSync } from "node:fs";
import { findFdtOffsets } from "./fdt.mjs";
const buf = readFileSync(process.argv[2]);
console.log("file size:", buf.length);
const hits = findFdtOffsets(buf);
console.log("FDT magic hits:", hits.length);
for (const h of hits.slice(0, 50)) {
  console.log(`  offset=0x${h.offset.toString(16)} size=${h.size}`);
}
console.log("head:", buf.subarray(0, 48).toString("hex"));
