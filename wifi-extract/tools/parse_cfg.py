#!/usr/bin/env python3
"""解析 popsicle WiFi 端点 (17cb:110e) 的 4 KiB PCI config space。"""
import sys, struct

# 用法: python parse_cfg.py <ep-cfg.bin>
# ep-cfg.bin 从真机取: adb shell su -c 'dd if=/sys/bus/pci/devices/0000:01:00.0/config of=/data/local/tmp/ep-cfg.bin bs=4096 count=1'
d = open(sys.argv[1] if len(sys.argv) > 1 else 'ep-cfg.bin', 'rb').read()
assert len(d) == 4096

def w(o): return struct.unpack_from('<H', d, o)[0]
def l(o): return struct.unpack_from('<I', d, o)[0]

print('=== 头部 ===')
print('vendor     0x%04x' % w(0x00))
print('device     0x%04x' % w(0x02))
print('command    0x%04x  (mem=%d bm=%d serr=%d)' % (w(0x04), w(0x04)&2>>1, w(0x04)&4>>2, w(0x04)&0x100>>8))
print('status     0x%04x' % w(0x06))
print('revision   0x%02x' % d[0x08])
print('class      0x%06x' % (d[0x0b] << 16 | d[0x0a] << 8 | d[0x09]))
print('cacheline  0x%02x  latency 0x%02x  hdrtype 0x%02x' % (d[0x0c], d[0x0d], d[0x0e]))
print('subsys     0x%04x:0x%04x' % (w(0x2e), w(0x2c)))

print('\n=== BAR ===')
for i in range(6):
    off = 0x10 + i * 4
    v = l(off)
    if v == 0:
        print('BAR%d  unused' % i); continue
    if v & 1:
        print('BAR%d  0x%08x  I/O space' % (i, v & ~3)); continue
    typ = (v >> 1) & 3
    if typ == 2 and i < 5:
        hi = l(off + 4)
        base = (hi << 32) | (v & ~0xf)
        print('BAR%d  0x%x  64-bit  %s' % (i, base, 'prefetch' if v & 8 else 'non-prefetch'))
    else:
        print('BAR%d  0x%08x  32-bit' % (i, v & ~0xf))

print('\n=== 能力链 ===')
cap = d[0x34] & 0xfc
seen = []
while cap and cap not in seen and cap < 0x100:
    seen.append(cap)
    cid = d[cap]
    nxt = d[cap + 1] & 0xfc
    names = {0x01: 'Power Management', 0x05: 'MSI', 0x10: 'PCI Express',
             0x11: 'MSI-X', 0x09: 'Vendor Specific', 0x0d: 'Subsystem VID',
             0x03: 'VPD', 0x0c: 'Power Budgeting'}
    print('0x%02x  id=0x%02x %-18s next=0x%02x' % (cap, cid, names.get(cid, '?'), nxt))
    if cid == 0x05:  # MSI
        mc = w(cap + 2)
        print('      MSI msgctrl=0x%04x enable=%d 64bit=%d pervec_mask=%d vectors=%d' %
              (mc, mc & 1, (mc >> 7) & 1, (mc >> 8) & 1, 1 << ((mc >> 4) & 7)))
        print('      addr=0x%016x' % ((l(cap + 8) << 32) | l(cap + 4)))
        print('      data=0x%08x' % l(cap + 12))
        if mc & 0x80:
            print('      mask=0x%08x' % l(cap + 16))
    if cid == 0x10:  # PCIe
        pcap = w(cap + 2)
        print('      devcap=0x%08x devctl=0x%04x devsta=0x%04x' % (l(cap + 4), w(cap + 8), w(cap + 10)))
        print('      lnkcap=0x%08x lnkctl=0x%04x lnksta=0x%04x' % (l(cap + 12), w(cap + 16), w(cap + 18)))
        ls = w(cap + 18)
        spd = {1: 2.5, 2: 5.0, 3: 8.0, 4: 16.0}.get(ls & 0xf, 0)
        lc = l(cap + 12)
        mspd = {1: 2.5, 2: 5.0, 3: 8.0, 4: 16.0}.get(lc & 0xf, 0)
        print('      -> 当前 %.1f GT/s x%d，最大 %.1f GT/s x%d' % (spd, (ls >> 4) & 0x3f, mspd, (lc >> 4) & 0x3f))
    if cid == 0x11:  # MSI-X
        mc = w(cap + 2)
        toff = l(cap + 4)
        poff = l(cap + 8)
        print('      MSI-X msgctrl=0x%04x enable=%d mask=%d table_size=%d' %
              (mc, mc & 1, (mc >> 14) & 1, (mc & 0x7ff) + 1))
        print('      table  BAR%d + 0x%x' % (toff & 7, toff & ~7))
        print('      PBA    BAR%d + 0x%x' % (poff & 7, poff & ~7))
    cap = nxt

print('\n=== 扩展能力（0x100 起）===')
off = 0x100
n = 0
while off and off < 0x1000 and n < 20:
    cid = w(off)
    ver = cid & 0xf
    cid >>= 4
    nxt = w(off + 2) >> 4
    xnames = {0x0001: 'AER', 0x0002: 'VC', 0x0003: 'Device Serial Number',
              0x0004: 'Power Budgeting', 0x000b: 'Vendor Specific',
              0x000d: 'ACS', 0x0015: 'SR-IOV', 0x0019: 'Secondary PCIe',
              0x001a: 'ATS', 0x001f: 'PRI', 0x0029: 'TPH'}
    print('0x%03x  id=0x%04x %-22s ver=%d next=0x%03x' % (off, cid, xnames.get(cid, '?'), ver, nxt))
    if cid == 0x0001:
        print('      uncorr_sta=0x%08x uncorr_mask=0x%08x uncorr_sev=0x%08x' %
              (l(off + 4), l(off + 8), l(off + 12)))
        print('      corr_sta=0x%08x corr_mask=0x%08x cap=0x%08x' %
              (l(off + 16), l(off + 20), l(off + 24)))
    off = nxt if nxt else 0
    n += 1
