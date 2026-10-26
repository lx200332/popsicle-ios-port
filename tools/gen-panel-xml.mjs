// Emit DisplayDxe panel XML for the popsicle primary and secondary panels.
import fs from 'node:fs';

const DTB = 'C:\\dsh-对话\\popsicle-port\\dts\\popsicle_merged.dtb';
const OUTDIR = 'C:\\dsh-对话\\popsicle-port\\panels';

const PANELS = [
  { node: 'qcom,mdss_dsi_p2_42_0d_0b_mp_prim_dsc_cmd', timing: 'timing@wqhd_normal_120hz_index_00', file: 'Panel_p2_42_0d_0b_mp_prim_dsc_cmd.xml', desc: 'xiaomi p2 42 0d 0b prim cmd mode dsc dsi panel' },
  { node: 'qcom,mdss_dsi_p2_42_06_0a_sec_dsc_cmd', timing: null, file: 'Panel_p2_42_06_0a_sec_dsc_cmd.xml', desc: 'xiaomi p2 42 06 0a sec cmd mode dsc dsi panel' },
];

const d = fs.readFileSync(DTB);
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
    nodes.push({ path: '/' + stack.filter(Boolean).join('/'), name, props: {} });
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

const u32 = b => (b && b.length === 4) ? b.readUInt32BE(0) : null;
const str = b => b ? b.toString('latin1').replace(/\0.*$/s, '') : '';
const has = (o, k) => k in o;

function decodeCommands(buf) {
  const out = [];
  let i = 0;
  while (i + 7 <= buf.length) {
    const type = buf[i];
    const pl = (buf[i + 5] << 8) | buf[i + 6];
    const payload = buf.subarray(i + 7, i + 7 + pl);
    out.push([type, ...payload]);
    i += 7 + pl;
  }
  return out;
}
const hexLine = arr => arr.map(b => b.toString(16).padStart(2, '0')).join(' ');

// DSIInterface.InterfaceType for DisplayDxe: 9 = DSI (per the nt37801 reference XML).
// DSITrafficMode: 1 = burst mode (0 = non-burst, 2 = video).
const trafficMode = s => s === 'non_burst_sync_pulse' ? 0 : s === 'burst_mode' ? 1 : s === 'non_burst_sync_event' ? 0 : 1;
// DSIColorFormat in the reference XML: 36 for 8bpc RGB888 on 4 lanes; 30bpp panels use 36 too in
// Qualcomm's UEFI driver (it means "RGB888, packed"), DSC carries the real depth.
const colorFormat = 36;

fs.mkdirSync(OUTDIR, { recursive: true });

for (const p of PANELS) {
  const panel = nodes.find(n => n.name === p.node);
  if (!panel) { console.log(`!! ${p.node} missing`); continue; }
  const kids = nodes.filter(n => n.path.startsWith(panel.path + '/'));
  const timings = kids.filter(k => k.name.startsWith('timing@'));
  const t = p.timing ? timings.find(k => k.name === p.timing) : timings[0];

  const g = k => u32(t.props[k]);
  const gp = k => u32(panel.props[k]);

  const W = g('qcom,mdss-dsi-panel-width');
  const H = g('qcom,mdss-dsi-panel-height');
  const fps = g('qcom,mdss-dsi-panel-framerate');
  const hfp = g('qcom,mdss-dsi-h-front-porch'), hbp = g('qcom,mdss-dsi-h-back-porch');
  const hpw = g('qcom,mdss-dsi-h-pulse-width'), hsk = g('qcom,mdss-dsi-h-sync-skew') ?? 0;
  const vfp = g('qcom,mdss-dsi-v-front-porch'), vbp = g('qcom,mdss-dsi-v-back-porch');
  const vpw = g('qcom,mdss-dsi-v-pulse-width'), vsk = g('qcom,mdss-dsi-v-sync-skew') ?? 0;
  const lanes = ['qcom,mdss-dsi-lane-0-state','qcom,mdss-dsi-lane-1-state','qcom,mdss-dsi-lane-2-state','qcom,mdss-dsi-lane-3-state']
    .filter(k => has(panel.props, k)).length;
  const dscEn = has(t.props, 'qcom,mdss-dsc-version');
  const sliceW = g('qcom,mdss-dsc-slice-width'), sliceH = g('qcom,mdss-dsc-slice-height');
  const dscVer = g('qcom,mdss-dsc-version');
  const scr = g('qcom,mdss-dsc-scr-version') ?? 0;
  const bpp8 = g('qcom,mdss-dsc-bit-per-pixel');
  const on = decodeCommands(t.props['qcom,mdss-dsi-on-command'] || Buffer.alloc(0));
  const off = decodeCommands(t.props['qcom,mdss-dsi-off-command'] || Buffer.alloc(0));
  const clockrate = g('qcom,mdss-dsi-panel-clockrate');

  const dscMajor = dscVer >= 17 ? 1 : 1;
  const dscMinor = dscVer === 18 ? 2 : dscVer === 17 ? 1 : 0;

  const xml = `<?xml version="1.0" encoding="utf-8"?>
<!-- Generated from popsicle_merged.dtb by tools/gen-panel-xml.mjs. Do not hand-edit.
     source node: ${p.node}
     timing node: ${t.name} -->
<PanelName>${p.node.replace(/^qcom,mdss_dsi_/, '')}</PanelName>
<PanelDescription>${p.desc} (${W}x${H} ${fps}Hz ${lanes}lane)</PanelDescription>
<Group id="Active Timing">
 <HorizontalActive>${W}</HorizontalActive>
 <HorizontalFrontPorch>${hfp}</HorizontalFrontPorch>
 <HorizontalBackPorch>${hbp}</HorizontalBackPorch>
 <HorizontalSyncPulse>${hpw}</HorizontalSyncPulse>
 <HorizontalSyncSkew>${hsk}</HorizontalSyncSkew>
 <HorizontalLeftBorder>0</HorizontalLeftBorder>
 <HorizontalRightBorder>0</HorizontalRightBorder>
 <VerticalActive>${H}</VerticalActive>
 <VerticalFrontPorch>${vfp}</VerticalFrontPorch>
 <VerticalBackPorch>${vbp}</VerticalBackPorch>
 <VerticalSyncPulse>${vpw}</VerticalSyncPulse>
 <VerticalSyncSkew>${vsk}</VerticalSyncSkew>
 <VerticalTopBorder>0</VerticalTopBorder>
 <VerticalBottomBorder>0</VerticalBottomBorder>
 <InvertDataPolarity>False</InvertDataPolarity>
 <InvertVsyncPolairty>False</InvertVsyncPolairty>
 <InvertHsyncPolarity>False</InvertHsyncPolarity>
 <BorderColor>0x0</BorderColor>
</Group>
<Group id="Display Interface">
 <InterfaceType>9</InterfaceType>
 <InterfaceColorFormat>3</InterfaceColorFormat>
</Group>
<Group id="DSI Interface">
 <DSIChannelId>2</DSIChannelId>
 <DSIVirtualId>${gp('qcom,mdss-dsi-virtual-channel-id') ?? 0}</DSIVirtualId>
 <DSIColorFormat>${colorFormat}</DSIColorFormat>
 <DSITrafficMode>${trafficMode(str(panel.props['qcom,mdss-dsi-traffic-mode']))}</DSITrafficMode>
 <DSILanes>${lanes}</DSILanes>
 <DSICmdSwapInterface>False</DSICmdSwapInterface>
 <DSITECheckEnable>${has(panel.props, 'qcom,mdss-dsi-te-pin-select') ? 'True' : 'False'}</DSITECheckEnable>
 <DSITEUsingDedicatedTEPin>True</DSITEUsingDedicatedTEPin>
 <DSITEvSyncStartPos>${H}</DSITEvSyncStartPos>
 <DSITEvSyncInitVal>${H}</DSITEvSyncInitVal>
 <DSIDSCEnable>${dscEn ? 'True' : 'False'}</DSIDSCEnable>
 <DSIDSCMajor>${dscMajor}</DSIDSCMajor>
 <DSIDSCMinor>${dscMinor}</DSIDSCMinor>
 <DSIDSCScr>${scr}</DSIDSCScr>
 <DSIDSCProfileID>${bpp8 === 8 ? 7 : 9}</DSIDSCProfileID>
 <DSIDSCSliceWidth>${sliceW}</DSIDSCSliceWidth>
 <DSIDSCSliceHeight>${sliceH}</DSIDSCSliceHeight>
 <DSIHsaHseAfterVsVe>False</DSIHsaHseAfterVsVe>
 <DSILowPowerModeInHFP>${has(panel.props, 'qcom,mdss-dsi-bllp-power-mode') ? 'True' : 'False'}</DSILowPowerModeInHFP>
 <DSILowPowerModeInHBP>${has(panel.props, 'qcom,mdss-dsi-bllp-power-mode') ? 'True' : 'False'}</DSILowPowerModeInHBP>
 <DSILowPowerModeInHSA>${has(panel.props, 'qcom,mdss-dsi-bllp-power-mode') ? 'True' : 'False'}</DSILowPowerModeInHSA>
 <DSILowPowerModeInBLLPEOF>${has(panel.props, 'qcom,mdss-dsi-bllp-eof-power-mode') ? 'True' : 'False'}</DSILowPowerModeInBLLPEOF>
 <DSILowPowerModeInBLLP>${has(panel.props, 'qcom,mdss-dsi-bllp-power-mode') ? 'True' : 'False'}</DSILowPowerModeInBLLP>
 <DSIRefreshRate>0x${(Math.round(clockrate / 1000) || 0x780000).toString(16)}</DSIRefreshRate>
 <DSICmdUsingTrigger>False</DSICmdUsingTrigger>
 <DSIEnableAutoRefresh>True</DSIEnableAutoRefresh>
 <DSIAutoRefreshFrameNumDiv>1</DSIAutoRefreshFrameNumDiv>
 <DSIControllerMapping>
  00
 </DSIControllerMapping>
</Group>
<DSIInitSequence>
${on.map(c => '  ' + hexLine(c)).join('\n')}
</DSIInitSequence>
<DSITermSequence>
${off.map(c => '  ' + hexLine(c)).join('\n')}
</DSITermSequence>
<Group id='Backlight Configuration'>
 <BacklightType>3</BacklightType>
 <BacklightBitWidth>12</BacklightBitWidth>
 <DisplayResetInfo>0 10 1000 10000 0</DisplayResetInfo>
</Group>
`;
  fs.writeFileSync(`${OUTDIR}\\${p.file}`, xml);
  console.log(`wrote ${p.file}: ${W}x${H} ${fps}Hz lanes=${lanes} dsc=${dscEn} ${on.length} init / ${off.length} term commands, ${xml.length} bytes`);
}
