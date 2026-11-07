#!/usr/bin/env python3
"""给预链接后的内核打 AMFI 签名校验绕过补丁。

    python3 patch_amfi.py prelinked_in prelinked_out [--check] [--profile P1P2]

原厂 AMFI 在签名校验里检查「CT policy」是否适配当前平台，不匹配就拒绝签名 ——
在我们的机器上这会把 NativeAdrenoMetal（GPU 插件）判成不合格，GPU 插件装不上，
整个系统退回 CPU 合成，于是屏幕只有 3~4 fps。

补丁站点（全部落在预链接后镜像的 __PLK_TEXT_EXEC，即原厂
com.apple.driver.AppleMobileFileIntegrity kext 的 __TEXT_EXEC）：

    P1  0xfffffff0047057b8  0xa90073f3 -> 0x14000004  跳过 CT policy 拒绝日志块
    P2  0xfffffff0047057dc  0xa90073f3 -> 0x14000004  跳过 developer-mode 拒绝日志块
    P3  0xfffffff0047053e4  0x52800036 -> 0x52800016  漏斗总闸 mov w22,#1 -> #0
    P4  0xfffffff004705328  0x52800036 -> 0x52800016  无日志拒绝预置 mov w22,#1 -> #0
    P5  0xfffffff004705dcc  0x52800036 -> 0x52800016  bail 块拒绝置位 mov w22,#1 -> #0
    P6  0xfffffff004705420  0x54002221 -> 0xd503201f  nop 掉 cs_flags 断言分支
    M1  0xfffffff004705450  0xaa1603e0 -> 0xd2800000  mov x0,x22 -> mov x0,#0（一条覆盖全部）

profile（也可用环境变量 AMFI_PROFILE 指定，命令行 --profile 优先）：
    P1P2          （默认）保守：只关掉两条拒绝日志块
    P1P2P3P4P6    彻底：另关漏斗总闸 + 无日志拒绝 + 断言
    P1P2P3P4P5P6  全关（P5 会把四类签名失败一并放行，风险最高）
    M1            最粗暴：无条件返回 0
    M1P6          M1 + 断言保护

重要：P1 与 P2 必须成对打 —— P1 的落点 0x47057c8 是 developer-mode 门的评估块，
其拒绝块 0x47057dc 紧贴其后；只打 P1 会在门判定失败时落进未打补丁的 P2，返回值仍为 1。

2026-11-04 事故：新流水线从零写起时漏了这一步，所有新镜像都没有这个补丁，
表现是「能进桌面但极卡」，日志里 AMFI 反复报 unsuitable CT policy。
"""
import os
import struct
import sys

SITES = {
    'P1': (0xfffffff0047057b8, 0xa90073f3, 0x14000004),
    'P2': (0xfffffff0047057dc, 0xa90073f3, 0x14000004),
    'P3': (0xfffffff0047053e4, 0x52800036, 0x52800016),
    'P4': (0xfffffff004705328, 0x52800036, 0x52800016),
    'P5': (0xfffffff004705dcc, 0x52800036, 0x52800016),
    'P6': (0xfffffff004705420, 0x54002221, 0xd503201f),
    'M1': (0xfffffff004705450, 0xaa1603e0, 0xd2800000),
}

PROFILES = {
    'P1P2': ['P1', 'P2'],
    'P1P2P3P4P6': ['P1', 'P2', 'P3', 'P4', 'P6'],
    'P1P2P3P4P5P6': ['P1', 'P2', 'P3', 'P4', 'P5', 'P6'],
    'M1': ['M1'],
    'M1P6': ['M1', 'P6'],
}

DEFAULT_PROFILE = 'P1P2'


def segments(d):
    magic, = struct.unpack_from('<I', d, 0)
    if magic != 0xfeedfacf:
        raise SystemExit(f'not a 64-bit Mach-O: {magic:#x}')
    ncmds, = struct.unpack_from('<I', d, 16)
    off = 32
    segs = []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            name = d[off + 8:off + 24].split(b'\0')[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', d, off + 24)
            segs.append((name, vmaddr, vmsize, fileoff, filesize))
        off += size
    return segs


def va_to_off(segs, va):
    for name, vmaddr, vmsize, fileoff, filesize in segs:
        if vmaddr <= va < vmaddr + vmsize:
            delta = va - vmaddr
            if delta >= filesize:
                return None, name
            return fileoff + delta, name
    return None, None


def parse_profile(argv):
    profile = os.environ.get('AMFI_PROFILE') or DEFAULT_PROFILE
    if '--profile' in argv:
        i = argv.index('--profile')
        if i + 1 >= len(argv):
            raise SystemExit('--profile needs a value: ' + ', '.join(PROFILES))
        profile = argv[i + 1]
    if profile not in PROFILES:
        raise SystemExit(f'unknown profile {profile!r}; choose from {", ".join(PROFILES)}')
    return profile


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    check_only = '--check' in sys.argv
    profile = parse_profile(sys.argv)
    names = PROFILES[profile]

    d = bytearray(open(src, 'rb').read())
    segs = segments(d)
    print(f'profile {profile}: {" ".join(names)}')
    ok = True
    for name in names:
        va, old, new = SITES[name]
        off, segname = va_to_off(segs, va)
        if off is None:
            print(f'FAIL {name} {va:#x} not in a file-backed segment ({segname})')
            ok = False
            continue
        cur, = struct.unpack_from('<I', d, off)
        if cur == new:
            print(f'PASS {name} {va:#x} already patched ({segname}+{off:#x})')
        elif cur == old:
            if check_only:
                print(f'FAIL {name} {va:#x} still unpatched ({segname}+{off:#x})')
                ok = False
            else:
                struct.pack_into('<I', d, off, new)
                print(f'PATCH {name} {va:#x} {old:#010x} -> {new:#010x} ({segname}+{off:#x})')
        else:
            print(f'FAIL {name} {va:#x} unexpected bytes {cur:#010x} ({segname}+{off:#x})')
            ok = False
    if not check_only:
        open(dst, 'wb').write(d)
        print(f'wrote {dst}')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
