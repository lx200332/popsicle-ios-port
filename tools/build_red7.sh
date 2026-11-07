#!/bin/bash
# build_red7.sh -- loader-red7-boot.img
#   native = /root/build/xnu141/prelink/kernel.plk141.d8
#   相比 red5: 内核换成 d8 (含 pandora_bt + pandora_modem);
#               cmdline 去掉 pandora_amp=1 (15 B), 加 pandora_modem=2 (16 B) -> 605 B
#   相比 red2：ADT 换成 phone-fwall.adt —— pandora-fw-extents 指向新的合并固件包
#              （allpack.img，58 项，42,188,800 B，10,300 块）
#   ADT 必须先由 fwpin.sh 在设备上 pin 出真实 extents 后生成；
#   若 phone-fwall.adt 不存在则以 phone-mac.adt 代替（extents 仍是旧 gpupack）。
set -euo pipefail
C=/mnt/c/dsh-tools/cache/pandora-ci
REPO=/root/build/ios-pandora
O=$C/build
W=/root/build/work
SRC=/root/build/xnu141/prelink/kernel.plk141.d9
K=$W/plk/kernel.red7
ADT=$O/phone-fwall.adt
[ -f "$ADT" ] || { echo "WARN: $ADT 不存在，回退 phone-mac.adt（fw-extents 仍是旧 gpupack）"; ADT=$O/phone-mac.adt; }
TC=$W/tc
NAME=red7

# red3 相对 red2 的变化：
#   + pandora_adsp=2 / pandora_files=1 / pandora_amp=1（首次启用 ADSP + 固件落盘 + 功放）
#   + pandora_bt=7（蓝牙 probe+power+uart）
#   sweetfb_hz 30 -> 60
#   为腾出 608 B 预算删掉：-noprogress（12 B）、wdt_test=no_user_checkin（25 B）、
#                           sweetnull 里的 keybagd（8 B）、securityd（10 B）
#   red4 又删 sweetnull 里的 lsd（4 B）：真实上限 607 B，608 B 字段的最后 1 B 是 NUL
CMDLINE='debug=0x8 serial=3 cpus=8 keepsyms=1 rd=ufsroot0 ufsrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=60 sweetfb_fill=1 sweetfb_noconsole=1 amfi_get_out_of_my_way=1 cs_enforcement_disable=1 sweetnull=SpringBoard,containermanagerd,backboardd pandora_touch=0xf pandora_logsecs=1 pandora_reboot_after=1800 pandora_gpu=15 pandora_gpu_metal=1 sweetiolog=lsd,SpringBoard sweetnoexec=bluetoothd,fairplayd.H2 sweetsample=containermanagerd,installd,lsd,SpringBoard fuxiwlan=1 pandora_adsp=2 pandora_files=1 pandora_bt=7 pandora_modem=2 pandora_thermal=1'

echo "=== 0 cmdline ==="
N=$(echo -n "$CMDLINE" | wc -c)
echo "cmdline $N B / 608   余 $((608-N)) B"
[ "$N" -le 608 ] || { echo CMDLINE_TOO_LONG; exit 1; }

echo
echo "=== 1 R1/R2 AMFI CT-policy 机器码补丁 ==="
/root/emu-venv/bin/python3 - "$SRC" "$K" <<'PY'
import struct, sys, hashlib
src, out = sys.argv[1], sys.argv[2]
d = bytearray(open(src, "rb").read())
ncmds, = struct.unpack_from("<I", d, 16); off = 32; S = []
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<2I", d, off)
    if cmd == 0x19:
        n = d[off+8:off+24].rstrip(b"\0").decode()
        va, vs, fo, fs = struct.unpack_from("<4Q", d, off+24); S.append((n, va, vs, fo, fs))
    off += size
def va2fo(v):
    for n, sva, vs, fo, fs in S:
        if sva <= v < sva + fs: return fo + (v - sva)
    return None
PATCH = [
    ("R1", 0xfffffff0047057b8, 0xa90073f3, 0x14000004, "CT policy reject block -> b pass path"),
    ("R2", 0xfffffff0047057dc, 0xa90073f3, 0x14000004, "developer-signature reject -> b pass path"),
]
ok = 0
for tag, va, want, new, why in PATCH:
    fo = va2fo(va)
    assert fo is not None, tag
    cur = struct.unpack_from("<I", d, fo)[0]
    print("  %s va=%#x fo=%#x cur=%#010x want=%#010x %s" % (tag, va, fo, cur, want, "OK" if cur == want else "MISMATCH"))
    if cur != want: raise SystemExit("MISMATCH " + tag)
    struct.pack_into("<I", d, fo, new)
    back = struct.unpack_from("<I", d, fo)[0]
    print("       wrote %#010x readback %#010x %s  (%s)" % (new, back, "PASS" if back == new else "FAIL", why))
    ok += back == new
open(out, "wb").write(d)
print("  VERDICT:", "ALL PASS" if ok == len(PATCH) else "FAIL")
print("  out %s  %d B  sha256 %s" % (out, len(d), hashlib.sha256(d).hexdigest()))
PY

echo
echo "=== 2 关键串复核 ==="
/root/emu-venv/bin/python3 - "$K" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
for s in [b"PANDORA CS:", b"FUXI WLAN: enabled", b"fuxiwlan=", b"PANDORA GPU UC",
          b"PANDORA TOUCH", b"PANDORA ADSP", b"PANDORA FILES", b"PANDORA FW:", b"SWEET AKS",
          b"AppleMobileFileIntegrityUserClient"]:
    print("  %-34s %d" % (s.decode(), d.count(s)))
PY

echo
echo "=== 3 payload ==="
python3 "$REPO/tools/mkpayload-real.py" "$K" "$ADT" "$O/payload-$NAME-nopre.bin" --cmdline "$CMDLINE" | tail -4

echo
echo "=== 4 trust cache ==="
python3 "$REPO/tools/addtrustcache.py" "$O/payload-$NAME-nopre.bin" "$O/payload-$NAME.bin" \
  "$TC/087-86698-044.dmg.trustcache" \
  "$TC/087-86593-047.dmg.trustcache+$TC/087-86715-047.dmg.trustcache+$TC/pandora-native-gpu.tc"

echo
echo "=== 5 loader 1200x2608 ==="
FB_W=1200 FB_H=2608 bash "$C/build_loader2.sh" "$O/payload-$NAME.bin" "$O/loader-$NAME-raw.img" | tail -4

echo
echo "=== 6 dyld shard cdhash 替换 ==="
/root/emu-venv/bin/python3 /mnt/c/dsh-tools/cache/diag/tc_fix_arg.py "$O/loader-$NAME-raw.img" "$O/loader-$NAME-boot.img" | tail -8

echo
echo "=== 7 终检 ==="
/root/emu-venv/bin/python3 - "$O/loader-$NAME-boot.img" "$CMDLINE" "$ADT" <<'PY'
import struct, sys, hashlib
p, want_cmd, adt = sys.argv[1], sys.argv[2], sys.argv[3]
d = open(p, "rb").read()
PL = None
for off in range(0x4000, min(len(d), 0x20000), 0x1000):
    if d[off:off+8] == b"SWEETXNU":
        PL = off; break
assert PL is not None
ver, nsegs = struct.unpack_from("<II", d, PL+8)
vb, entry, adt_off, adt_size, kend = struct.unpack_from("<QQQII", d, PL+0x10)
pm = d[PL+0x30:PL+0x38]
ph, me = struct.unpack("<QQ", d[PL+0x38:PL+0x48])
cm = d[PL+0x48:PL+0x48+608].split(b"\0")[0].decode()
print("  payload@0x%x ver=%d nsegs=%d" % (PL, ver, nsegs))
print("  entry=0x%x kend=0x%x phys=0x%x mem=0x%x adt=%d" % (entry, kend, ph, me, adt_size))
print("  cmdline(%d B) matches: %s" % (len(cm), cm == want_cmd))
for k in ("fuxiwlan=1", "pandora_adsp=2", "pandora_files=1", "pandora_amp=1", "sweetfb_hz=60"):
    print("    %-20s %s" % (k, k in cm))
NEW = bytes.fromhex("446897ac71310eedb24ad417502274bf37bd9627")
print("  plugin new cdhash count =", d.count(NEW))
print("  size =", len(d))
print("  sha256 =", hashlib.sha256(d).hexdigest())
print("  adt =", adt)
PY
