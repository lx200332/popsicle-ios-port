// Extract qcom,mdss-dsi-panel-name (and related) from every mdss_dsi panel node in the merged tree.
import fs from 'node:fs';

const d = fs.readFileSync('C:\\dsh-对话\\popsicle-port\\dts\\popsicle_merged.dtb');
const be32 = o => d.readUInt32BE(o);
const offStruct = be32(8), offStrings = be32(12);

let off = offStruct;
const stack = [];
const nodes = [];
while (off < d.length) {
  const tok = be32(off); off += 4;
  if (tok === 1) {
    let e = off; while (d[e] !== 0) e++;
    const name = d.subarray(off, e).toString('latin1');
    off = (e + 4) & ~3;
    stack.push(name);
    nodes.push({ path: '/' + stack.filter(Boolean).join('/'), props: {} });
  } else if (tok === 2) stack.pop();
  else if (tok === 3) {
    const len = be32(off), nameOff = be32(off + 4); off += 8;
    let s = offStrings + nameOff, e = s; while (d[e] !== 0) e++;
    const pname = d.subarray(s, e).toString('latin1');
    nodes[nodes.length - 1].props[pname] = d.subarray(off, off + len);
    off = (off + len + 3) & ~3;
  } else if (tok === 4) continue;
  else break;
}

const rows = [];
for (const n of nodes) {
  const pn = n.props['qcom,mdss-dsi-panel-name'];
  if (!pn) continue;
  const str = b => b ? b.toString('latin1').replace(/\0.*$/s, '') : '';
  rows.push({
    node: n.path.split('/').pop(),
    name: str(pn),
    model: str(n.props['qcom,mdss-dsi-panel-model']),
    type: str(n.props['qcom,mdss-dsi-panel-type']),
    w: n.props['qcom,mdss-dsi-panel-width'] ? n.props['qcom,mdss-dsi-panel-width'].readUInt32BE(0) : null,
    h: n.props['qcom,mdss-dsi-panel-height'] ? n.props['qcom,mdss-dsi-panel-height'].readUInt32BE(0) : null,
    status: str(n.props['status']),
  });
}

console.log(`panel nodes with qcom,mdss-dsi-panel-name: ${rows.length}\n`);
for (const r of rows) {
  console.log(`${r.name}`);
  console.log(`   node=${r.node}  ${r.w}x${r.h}  type=${r.type}  model=${r.model}  status=${r.status}`);
}
