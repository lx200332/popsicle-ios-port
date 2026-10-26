#!/usr/bin/env node
// FDT (Flattened Device Tree, .dtb) 解析 + DTS 反编译
// 纯 Node 实现，不依赖 dtc
import { readFileSync, writeFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const MAGIC = 0xd00dfeed;
const T_BEGIN_NODE = 1, T_END_NODE = 2, T_PROP = 3, T_NOP = 4, T_END = 9;
const align4 = n => (n + 3) & ~3;

function cstr(buf, start, limit) {
  let e = start;
  const lim = limit === undefined ? buf.length : Math.min(limit, buf.length);
  while (e < lim && buf[e] !== 0) e++;
  return buf.subarray(start, e).toString('utf8');
}

export function parseFdt(buf) {
  if (buf.length < 40) throw new Error('too small for FDT header');
  const magic = buf.readUInt32BE(0);
  if (magic !== MAGIC) throw new Error(`bad FDT magic 0x${magic.toString(16)} (expected 0xd00dfeed)`);

  const totalsize = buf.readUInt32BE(4);
  const offStruct = buf.readUInt32BE(8);
  const offStrings = buf.readUInt32BE(12);
  const offRsvmap = buf.readUInt32BE(16);
  const version = buf.readUInt32BE(20);
  const lastComp = buf.readUInt32BE(24);
  const bootCpuid = buf.readUInt32BE(28);
  const sizeStrings = buf.readUInt32BE(32);
  const sizeStruct = buf.readUInt32BE(36);

  if (totalsize > buf.length) {
    // 有些镜像里 dtb 后面跟着别的东西，截断处理
  }

  // memory reservation block
  const memrsv = [];
  let p = offRsvmap;
  while (p + 16 <= buf.length) {
    const addr = buf.readBigUInt64BE(p);
    const size = buf.readBigUInt64BE(p + 8);
    if (addr === 0n && size === 0n) break;
    memrsv.push({ addr, size });
    p += 16;
  }

  const strings = buf.subarray(offStrings, offStrings + sizeStrings);
  const structEnd = Math.min(offStruct + sizeStruct, buf.length);

  const root = { name: '', props: [], children: [] };
  const stack = [root];
  let cur = root;
  let off = offStruct;

  while (off + 4 <= structEnd) {
    const tok = buf.readUInt32BE(off); off += 4;
    if (tok === T_BEGIN_NODE) {
      const name = cstr(buf, off, structEnd);
      off += align4(name.length + 1);
      const node = { name, props: [], children: [] };
      cur.children.push(node);
      stack.push(node);
      cur = node;
    } else if (tok === T_END_NODE) {
      if (stack.length > 1) stack.pop();
      cur = stack[stack.length - 1];
    } else if (tok === T_PROP) {
      const len = buf.readUInt32BE(off); off += 4;
      const nameoff = buf.readUInt32BE(off); off += 4;
      const pname = cstr(strings, nameoff);
      const value = Buffer.from(buf.subarray(off, off + len));
      off += align4(len);
      cur.props.push({ name: pname, value });
    } else if (tok === T_NOP) {
      // skip
    } else if (tok === T_END) {
      break;
    } else {
      throw new Error(`unknown FDT token 0x${tok.toString(16)} at offset ${off - 4}`);
    }
  }

  return { header: { totalsize, version, lastComp, bootCpuid, sizeStrings, sizeStruct }, memrsv, root };
}

// ---- 属性值格式化（模仿 dtc 的启发式） ----
function isPrintableRun(b) {
  for (const c of b) {
    if (c === 0x09 || c === 0x0a || c === 0x0d) continue;
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}

function asStringList(v) {
  if (v.length === 0 || v[v.length - 1] !== 0) return null;
  const parts = [];
  let s = 0;
  let nonEmpty = 0;
  for (let i = 0; i < v.length; i++) {
    if (v[i] === 0) {
      if (!isPrintableRun(v.subarray(s, i))) return null;
      const str = v.subarray(s, i).toString('utf8');
      if (str.length > 0) nonEmpty++;
      parts.push(str);
      s = i + 1;
    }
  }
  if (s !== v.length) return null;
  // 全零/全空串的 4 字节值应判为 cells，而非字符串
  if (nonEmpty === 0) return null;
  return parts;
}

export function fmtVal(v, maxCells = 4096) {
  if (!v || v.length === 0) return '';
  const sl = asStringList(v);
  if (sl) return sl.map(s => JSON.stringify(s)).join(', ');
  if (v.length % 4 === 0) {
    const n = v.length / 4;
    const cells = [];
    for (let i = 0; i < Math.min(n, maxCells); i++) cells.push('0x' + v.readUInt32BE(i * 4).toString(16));
    const tail = n > maxCells ? ` /* +${n - maxCells} more cells */` : '';
    return '<' + cells.join(' ') + '>' + tail;
  }
  const hex = [...v].map(x => x.toString(16).padStart(2, '0'));
  if (hex.length > 96) return `[${hex.slice(0, 96).join(' ')} /* ... ${v.length} bytes */]`;
  return '[' + hex.join(' ') + ']';
}

export function toDts(tree, { maxDepth = 64 } = {}) {
  const out = ['/dts-v1/;', ''];
  for (const r of tree.memrsv) {
    out.push(`/memreserve/ 0x${r.addr.toString(16)} 0x${r.size.toString(16)};`);
  }
  if (tree.memrsv.length) out.push('');

  const emit = (node, depth) => {
    if (depth > maxDepth) { out.push(`${'\t'.repeat(depth)}/* truncated */`); return; }
    const ind = '\t'.repeat(depth);
    const label = node.name === '' ? '/' : node.name;
    out.push(`${ind}${label} {`);
    for (const p of node.props) {
      const v = fmtVal(p.value);
      out.push(`${ind}\t${p.name}${v === '' ? '' : ' = ' + v};`);
    }
    for (const c of node.children) emit(c, depth + 1);
    out.push(`${ind}};`);
  };
  emit(tree.root, 0);
  return out.join('\n') + '\n';
}

// ---- 便捷：在 buffer 里搜索 FDT magic ----
export function findFdtOffsets(buf) {
  const hits = [];
  for (let i = 0; i + 40 <= buf.length; i += 4) {
    if (buf.readUInt32BE(i) !== MAGIC) continue;
    const total = buf.readUInt32BE(i + 4);
    if (total < 40 || i + total > buf.length) continue;
    hits.push({ offset: i, size: total });
    i += total - 4;
  }
  return hits;
}

// CLI
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [inp, outp] = process.argv.slice(2);
  if (!inp) { console.error('usage: node fdt.mjs <in.dtb> [out.dts]'); process.exit(2); }
  const buf = readFileSync(inp);
  const t = parseFdt(buf);
  const dts = toDts(t);
  if (outp) { writeFileSync(outp, dts); console.log(`wrote ${outp} (${dts.length} bytes)`); }
  else process.stdout.write(dts);
}
