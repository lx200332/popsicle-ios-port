import struct, os, sys
# 用法: python decode_cnss.py <解包后的 dtb 目录>/soc/qcom,cnss-peach@b0000000
D = sys.argv[1] if len(sys.argv) > 1 else "."
def rd(n):
    p = os.path.join(D, n)
    return open(p, 'rb').read() if os.path.exists(p) else None

def u32(name):
    d = rd(name)
    if not d: return None
    return struct.unpack('>%dI' % (len(d)//4), d[:len(d)//4*4])

print("== 标量 ==")
for n in ['qcom,wlan-rc-num','supported-ids','qcom,wlan-ramdump-dynamic','msix-match-addr',
          'qcom,bus-bw-cfg-count','qcom,icc-path-count','wlan-txrx-intr-cpumask']:
    v = u32(n)
    if v: print(" %-28s = %s" % (n, [hex(x) for x in v]))

print("\n== qcom,bus-bw-cfg (path, usecase, ...) ==")
v = u32('qcom,bus-bw-cfg')
if v:
    cnt = u32('qcom,bus-bw-cfg-count')[0]
    print("  总 u32 = %d, count=%d, 每项 = %d 个 u32" % (len(v), cnt, len(v)//cnt))
    per = len(v)//cnt
    for i in range(cnt):
        row = v[i*per:(i+1)*per]
        print("   [%d] %s" % (i, ' '.join('0x%x' % x for x in row)))

print("\n== interconnects ==")
v = u32('interconnects')
if v: print("  ", [hex(x) for x in v])

print("\n== PMU vreg map / pdc map (ascii) ==")
for n in ['qcom,pmu_vreg_map','qcom,vreg_pdc_map','qcom,pdc_mode_vote_table']:
    d = rd(n)
    if d: print(" %s:\n   %s" % (n, d.decode('latin1').replace('\x00',' ').strip()))

print("\n== vdd config 表 (min_uV max_uV ... enable) ==")
for n in ['qcom,vdd-wlan-cx-config','qcom,vdd-wlan-dig-config','qcom,vdd-wlan-rfa1-config',
          'qcom,vdd-wlan-rfa2-config','qcom,vdd-wlan-aon-config','qcom,vdd-wlan-io12-config',
          'qcom,vdd-wlan-ant-share-config']:
    v = u32(n)
    if v: print(" %-32s = %s" % (n, [hex(x) for x in v]))
