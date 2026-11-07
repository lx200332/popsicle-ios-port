#!/usr/bin/env python3
"""拓扑自检：内核需要双簇（补丁 0320 及以后）时，ADT 必须声明两个 CPU 簇。

    python3 topo_check.py IMAGE

2026-11-04 的事故就是这么来的：内核从 320 起把 CPU 分成两簇（六核 E + 两核 P），
而打包时 ADT 仍是单簇 → SpringBoard 卡死、屏幕全黑；两者只在最后一步才暴露，
所以把它变成打包时的硬性检查，不合就 FAIL，不许出图。

判定：payload 里出现 pandora_pspill（0320 引入的启动参数）即认为内核需要双簇；
此时 ADT 的 /device-tree/cpus/cpu@*/cluster-type 必须同时出现 E 和 P。
"""
import struct
import sys

PAYLOAD_IN_IMAGE = 0xB000


def tolerant_adt_module(repo):
    src = open(repo + '/tools/adt.py', encoding='utf-8').read()
    src = src.replace('.decode()', ".decode('utf-8', 'replace')")
    src = src.replace('.encode()', ".encode('utf-8', 'replace')")
    ns = {'__name__': 'adtx'}
    exec(compile(src, 'adtx', 'exec'), ns)
    return ns


def flatten(node, path='', out=None):
    if out is None:
        out = {}
    here = path + '/' + (node.name or '')
    for entry in node.props:
        out[(here, entry[0])] = entry[2]
    for child in node.children:
        flatten(child, here, out)
    return out


def main():
    repo = sys.argv[2] if len(sys.argv) > 2 else '.'
    d = open(sys.argv[1], 'rb').read()
    kernel_size, = struct.unpack_from('<I', d, 8)
    image = d[0x1000:0x1000 + kernel_size]
    p = image[PAYLOAD_IN_IMAGE:]
    adt_off, adt_size = struct.unpack_from('<2Q', p, 0x20)
    adt = p[adt_off:adt_off + adt_size]

    needs_dual = b'pandora_pspill' in p
    adt_ns = tolerant_adt_module(repo)
    root, _ = adt_ns['parse'](adt)
    props = flatten(root)

    types = sorted({v for (node, key), v in props.items()
                    if key == 'cluster-type' and '/cpus/' in node})
    has_e = any(v[:1] == b'E' for v in types)
    has_p = any(v[:1] == b'P' for v in types)

    print(f'kernel needs dual cluster: {needs_dual}')
    print(f'ADT cluster-type values: {[v[:1].decode(errors="replace") for v in types]}')
    if needs_dual and not (has_e and has_p):
        print('FAIL 内核是双簇内核，但 ADT 只有单簇 —— 会黑屏（2026-11-04 事故）')
        return 1
    print('PASS 拓扑与内核一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())
