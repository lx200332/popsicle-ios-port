#!/usr/bin/env node
// FDT overlay 合并器（纯 Node，不依赖 dtc / fdtoverlay）
//
// 用法:
//   node fdt-merge.mjs --info <overlay.dtb>
//   node fdt-merge.mjs <base.dtb> <overlay.dtb> <out.dtb>
//
// 实现标准 dtc overlay 语义:
//   1. 读 overlay 的 /__fixups__，把 base 里对应 label 节点的 phandle
//      写回 overlay 指定 path/prop/offset
//   2. 读 overlay 的 /__local_fixups__，把 overlay 内部 phandle 重编号到
//      base 之上，避免与 base 冲突
//   3. 逐个 /fragment@N: target phandle -> base 节点，把 /__overlay__
//      递归合并进去（属性覆盖，同名子节点递归）
import { readFileSync, writeFileSync } from 'node:fs';
import { parseFdt } from './fdt.mjs';

const MAGIC = 0xd00dfeed;
const T_BEGIN_NODE = 1, T_END_NODE = 2, T_PROP = 3, T_NOP = 4, T_END = 9;
const align4 = n => (n + 3) & ~3;

// ---------- 节点工具 ----------
const realRoot = t => t.root.children[0] ?? t.root;

function child(node, name) {
  return node.children.find(c => c.name === name);
}
function childAll(node, name) {
  return node.children.filter(c => c.name === name);
}
function getProp(node, name) {
  return node.props.find(p => p.name === name);
}
function setProp(node, name, value) {
  const p = getProp(node, name);
  if (p) p.value = value;
  else node.props.push({ name, value });
}
function delProp(node, name) {
  node.props = node.props.filter(p => p.name !== name);
}
function nodePath(root, target, path = '') {
  if (root === target) return path || '/';
  for (const c of root.children) {
    const r = nodePath(c, target, path + '/' + c.name);
    if (r) return r;
  }
  return null;
}
// 按 /a/b/c 路径查找（路径段与节点名精确匹配）
function byPath(root, path) {
  if (!path || path === '/') return root;
  const segs = path.split('/').filter(Boolean);
  let cur = root;
  for (const s of segs) {
    cur = cur.children.find(c => c.name === s);
    if (!cur) return null;
  }
  return cur;
}
function phandleOf(node) {
  const p = getProp(node, 'phandle') ?? getProp(node, 'linux,phandle');
  if (!p || p.value.length < 4) return null;
  return p.value.readUInt32BE(0);
}
function setPhandle(node, val) {
  const b = Buffer.alloc(4);
  b.writeUInt32BE(val, 0);
  setProp(node, 'phandle', b);
}
function walk(node, fn, path = '') {
  fn(node, path || '/');
  for (const c of node.children) walk(c, fn, path + '/' + c.name);
}

// ---------- 序列化 ----------
function buildFdt(root, memrsv = []) {
  const strings = [];
  const strIndex = new Map();
  const strOff = s => {
    if (strIndex.has(s)) return strIndex.get(s);
    const off = strings.reduce((a, x) => a + x.length + 1, 0);
    strIndex.set(s, off);
    strings.push(s);
    return off;
  };

  const chunks = [];
  let structLen = 0;
  const push = b => { chunks.push(b); structLen += b.length; };
  const u32 = v => { const b = Buffer.alloc(4); b.writeUInt32BE(v, 0); return b; };

  const emit = node => {
    push(u32(T_BEGIN_NODE));
    const nameBuf = Buffer.from(node.name + '\0', 'utf8');
    const namePad = Buffer.alloc(align4(nameBuf.length) - nameBuf.length);
    push(nameBuf); push(namePad);

    for (const p of node.props) {
      push(u32(T_PROP));
      push(u32(p.value.length));
      push(u32(strOff(p.name)));
      const pad = Buffer.alloc(align4(p.value.length) - p.value.length);
      push(p.value); push(pad);
    }
    for (const c of node.children) emit(c);
    push(u32(T_END_NODE));
  };

  // 文档根用空名
  const rootClone = { name: '', props: root.props, children: root.children };
  emit(rootClone);
  push(u32(T_END));

  const structBuf = Buffer.concat(chunks, structLen);
  const stringsBuf = Buffer.concat(strings.map(s => Buffer.from(s + '\0', 'utf8')));
  const rsv = memrsv.filter(r => !(r.addr === 0n && r.size === 0n));
  const rsvLen = 16 * (rsv.length + 1);
  const offRsvmap = 40; // header 本身正好 40 字节（0x28）
  const offStruct = offRsvmap + rsvLen;
  const offStrings = offStruct + structBuf.length;
  const totalsize = offStrings + stringsBuf.length;

  const hdr = Buffer.alloc(40);
  hdr.writeUInt32BE(MAGIC, 0);
  hdr.writeUInt32BE(totalsize, 4);
  hdr.writeUInt32BE(offStruct, 8);
  hdr.writeUInt32BE(offStrings, 12);
  hdr.writeUInt32BE(offRsvmap, 16);
  hdr.writeUInt32BE(17, 20);   // version
  hdr.writeUInt32BE(16, 24);   // last_comp_version
  hdr.writeUInt32BE(0, 28);    // boot_cpuid_phys
  hdr.writeUInt32BE(stringsBuf.length, 32);
  hdr.writeUInt32BE(structBuf.length, 36);

  const rsvBuf = Buffer.alloc(rsvLen);
  rsv.forEach((r, i) => {
    rsvBuf.writeBigUInt64BE(r.addr, i * 16);
    rsvBuf.writeBigUInt64BE(r.size, i * 16 + 8);
  });

  return Buffer.concat([hdr, rsvBuf, structBuf, stringsBuf], totalsize);
}

// ---------- fixup 解析 ----------
// 值形如 "/fragment@0:target:0\0/fragment@1:target:0\0"
function parseFixupRefs(value) {
  return value.toString('utf8').split('\0').filter(Boolean).map(s => {
    const i1 = s.indexOf(':');
    const i2 = s.lastIndexOf(':');
    return { path: s.slice(0, i1), prop: s.slice(i1 + 1, i2), offset: parseInt(s.slice(i2 + 1), 10) };
  });
}

// ---------- 主流程 ----------
function merge(baseT, ovT) {
  const base = realRoot(baseT);
  const ov = realRoot(ovT);

  // 1) base 的 label -> 节点
  const symNode = child(base, '__symbols__');
  const symbols = new Map();
  if (symNode) {
    for (const p of symNode.props) {
      const path = p.value.toString('utf8').replace(/\0+$/, '');
      const n = byPath(base, path);
      if (n) symbols.set(p.name, { node: n, path });
    }
  }

  // 2) base 现有 phandle 上界
  let maxPh = 0;
  walk(base, n => { const ph = phandleOf(n); if (ph && ph > maxPh) maxPh = ph; });

  // 3) 给 overlay 内部 phandle 重新编号（在 base 之上），并记录映射
  const localFix = child(ov, '__local_fixups__');
  const phMap = new Map();
  if (localFix) {
    // __local_fixups__ 的每个子树对应 overlay 里的一个节点
    walk(localFix, () => { });
    const renum = (fixNode, ovPath) => {
      const target = byPath(ov, ovPath);
      if (target) {
        const oldPh = phandleOf(target);
        if (oldPh != null) {
          const newPh = ++maxPh;
          phMap.set(oldPh, newPh);
          setPhandle(target, newPh);
        }
      }
      for (const c of fixNode.children) renum(c, (ovPath === '/' ? '' : ovPath) + '/' + c.name);
    };
    renum(localFix, '/');
    // 应用 overlay 内部引用
    const applyLocal = (fixNode, ovPath) => {
      const target = byPath(ov, ovPath);
      if (target) {
        for (const p of fixNode.props) {
          const prop = getProp(target, p.name);
          if (!prop) continue;
          const v = Buffer.from(prop.value);
          for (let off = 0; off + 4 <= p.value.length; off += 4) {
            const oldPh = p.value.readUInt32BE(off);
            const newPh = phMap.get(oldPh);
            if (newPh != null && off + 4 <= v.length) v.writeUInt32BE(newPh, off);
          }
          prop.value = v;
        }
      }
      for (const c of fixNode.children) applyLocal(c, (ovPath === '/' ? '' : ovPath) + '/' + c.name);
    };
    applyLocal(localFix, '/');
  }

  // 4) 应用 __fixups__: 把 base 节点的 phandle 写入 overlay
  const fixups = child(ov, '__fixups__');
  let fixupApplied = 0, fixupMissing = [];
  if (fixups) {
    for (const p of fixups.props) {
      const label = p.name;
      const sym = symbols.get(label);
      if (!sym) { fixupMissing.push(label); continue; }
      let ph = phandleOf(sym.node);
      if (ph == null) { ph = ++maxPh; setPhandle(sym.node, ph); }
      for (const ref of parseFixupRefs(p.value)) {
        const node = byPath(ov, ref.path);
        if (!node) continue;
        const prop = getProp(node, ref.prop);
        if (!prop || ref.offset + 4 > prop.value.length) continue;
        const v = Buffer.from(prop.value);
        v.writeUInt32BE(ph, ref.offset);
        prop.value = v;
        fixupApplied++;
      }
    }
  }

  // 5) 合并 fragment
  let merged = 0, skipped = [];
  const frags = ov.children.filter(c => c.name.startsWith('fragment@'));
  for (const frag of frags) {
    const ovl = child(frag, '__overlay__');
    if (!ovl) continue;
    const tgtProp = getProp(frag, 'target');
    if (!tgtProp || tgtProp.value.length < 4) { skipped.push(frag.name + ' (no target)'); continue; }
    const ph = tgtProp.value.readUInt32BE(0);
    let targetNode = null;
    walk(base, n => { if (!targetNode && phandleOf(n) === ph) targetNode = n; });
    if (!targetNode) { skipped.push(`${frag.name} (phandle 0x${ph.toString(16)} not in base)`); continue; }
    mergeInto(targetNode, ovl);
    merged++;
  }

  return { base, stats: { symbols: symbols.size, fixupApplied, fixupMissing, fragments: frags.length, merged, skipped } };
}

function mergeInto(dst, src) {
  for (const p of src.props) {
    if (p.name === 'phandle' || p.name === 'linux,phandle') continue;
    setProp(dst, p.name, p.value);
  }
  for (const sc of src.children) {
    const existing = child(dst, sc.name);
    if (existing) mergeInto(existing, sc);
    else dst.children.push({ name: sc.name, props: sc.props.map(p => ({ name: p.name, value: p.value })), children: sc.children });
  }
}

// ---------- CLI ----------
const args = process.argv.slice(2);
if (args[0] === '--info') {
  const t = parseFdt(readFileSync(args[1]));
  const r = realRoot(t);
  console.log('root children:', r.children.length);
  console.log('names:', r.children.map(c => c.name).slice(0, 12).join(' | '));
  for (const n of ['__fixups__', '__local_fixups__', '__symbols__']) {
    const c = child(r, n);
    if (c) console.log(`[${n}] props=${c.props.length} children=${c.children.length}`);
  }
  const frags = r.children.filter(c => c.name.startsWith('fragment@'));
  console.log('fragments:', frags.length);
} else if (args.length >= 3) {
  const baseT = parseFdt(readFileSync(args[0]));
  const ovT = parseFdt(readFileSync(args[1]));
  const { base, stats } = merge(baseT, ovT);

  // 合并已完成，overlay 簿记节点在运行期无用，剥掉可显著减小体积
  const before = base.children.length;
  base.children = base.children.filter(c => !['__symbols__', '__fixups__', '__local_fixups__'].includes(c.name));
  const stripped = before - base.children.length;

  // 设备标识：stock base 的 model/compatible 是 SoC 级（Canoe），换成设备名
  const devModel = process.env.MODEL;
  if (devModel) setProp(base, 'model', Buffer.from(devModel + '\0', 'utf8'));
  const devCompat = process.env.COMPATIBLE;
  if (devCompat) {
    const list = devCompat.split(',').map(s => s.trim()).filter(Boolean);
    setProp(base, 'compatible', Buffer.concat(list.map(s => Buffer.from(s + '\0', 'utf8'))));
  }

  const out = buildFdt(base, baseT.memrsv);
  writeFileSync(args[2], out);
  console.log('stats:', JSON.stringify({ ...stats, strippedMeta: stripped }, null, 2));
  console.log(`wrote ${args[2]} (${out.length} bytes)`);
  const check = parseFdt(out);
  console.log('verify: re-parsed ok, root children =', realRoot(check).children.length);
} else {
  console.error('usage: node fdt-merge.mjs --info <overlay.dtb>');
  console.error('       node fdt-merge.mjs <base.dtb> <overlay.dtb> <out.dtb>');
  process.exit(2);
}
