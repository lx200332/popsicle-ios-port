#!/usr/bin/env node
// Android boot / vendor_boot / dtbo 镜像解析与提取
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

const align = (n, a) => Math.ceil(n / a) * a;

function readCstr(buf, off, max) {
  let e = off;
  const lim = Math.min(off + max, buf.length);
  while (e < lim && buf[e] !== 0) e++;
  return buf.subarray(off, e).toString('utf8');
}

// ---------- boot.img (header v3 / v4) ----------
export function parseBoot(buf) {
  const magic = readCstr(buf, 0, 8);
  if (magic !== 'ANDROID!') throw new Error(`not a boot image (magic="${magic}")`);
  const kernel_size = buf.readUInt32LE(8);
  const ramdisk_size = buf.readUInt32LE(12);
  const os_version = buf.readUInt32LE(16);
  const header_size = buf.readUInt32LE(20);
  const header_version = buf.readUInt32LE(40);

  // v0/v1/v2 有 page_size，v3+ 固定 4096
  let page_size = 4096;
  let dtb_size = 0, dtb_addr = 0n;
  if (header_version < 3) {
    page_size = buf.readUInt32LE(36);
    if (header_version >= 2) {
      dtb_size = buf.readUInt32LE(1648);
      dtb_addr = buf.readBigUInt64LE(1652);
    }
  }
  const cmdline = header_version >= 3
    ? readCstr(buf, 44, 1536)
    : readCstr(buf, 64, 512) + readCstr(buf, 608, 1024);

  const hdrPages = align(header_size, page_size);
  const parts = [];
  let off = hdrPages;
  const add = (name, size) => {
    if (size > 0) parts.push({ name, offset: off, size });
    off += align(size, page_size);
  };
  add('kernel', kernel_size);
  add('ramdisk', ramdisk_size);
  if (header_version < 3) {
    add('second', buf.readUInt32LE(24));
    if (header_version === 1 || header_version === 2) {
      const rdtbo = buf.readUInt32LE(1632);
      add('recovery_dtbo', rdtbo);
    }
    if (header_version === 2) add('dtb', dtb_size);
  }
  return { kind: 'boot', header_version, header_size, page_size, cmdline, parts, dtb_size };
}

// ---------- vendor_boot.img (header v3 / v4) ----------
export function parseVendorBoot(buf) {
  const magic = readCstr(buf, 0, 8);
  if (magic !== 'VNDRBOOT') throw new Error(`not a vendor_boot image (magic="${magic}")`);
  const header_version = buf.readUInt32LE(8);
  const page_size = buf.readUInt32LE(12);
  const kernel_addr = buf.readUInt32LE(16);
  const ramdisk_addr = buf.readUInt32LE(20);
  const vendor_ramdisk_size = buf.readUInt32LE(24);
  const cmdline = readCstr(buf, 28, 2048);
  const tags_addr = buf.readUInt32LE(2076);
  const name = readCstr(buf, 2080, 16);
  const header_size = buf.readUInt32LE(2096);
  const dtb_size = buf.readUInt32LE(2100);
  const dtb_addr = buf.readBigUInt64LE(2104);

  const hdrPages = align(header_size, page_size);
  const vrOff = hdrPages;
  const dtbOff = vrOff + align(vendor_ramdisk_size, page_size);

  const parts = [
    { name: 'vendor_ramdisk', offset: vrOff, size: vendor_ramdisk_size },
  ];
  if (dtb_size > 0) parts.push({ name: 'dtb', offset: dtbOff, size: dtb_size });

  let tableOff = dtbOff + align(dtb_size, page_size);
  if (header_version >= 4) {
    const table_size = buf.readUInt32LE(2112);
    const entry_num = buf.readUInt32LE(2116);
    const entry_size = buf.readUInt32LE(2120);
    const bootconfig_size = buf.readUInt32LE(2124);
    if (table_size > 0) parts.push({ name: 'vendor_ramdisk_table', offset: tableOff, size: table_size });
    tableOff += align(table_size, page_size);
    if (bootconfig_size > 0) parts.push({ name: 'bootconfig', offset: tableOff, size: bootconfig_size });
  }

  return {
    kind: 'vendor_boot', header_version, header_size, page_size, cmdline, name,
    dtb_size, dtb_addr, parts,
  };
}

// ---------- dtbo.img (dt_table) ----------
export function parseDtbo(buf) {
  const magic = buf.readUInt32BE(0);
  if (magic !== 0xd7b7ab1e) throw new Error(`not a dtbo image (magic=0x${magic.toString(16)})`);
  const total_size = buf.readUInt32BE(4);
  const header_size = buf.readUInt32BE(8);
  const dt_entry_size = buf.readUInt32BE(12);
  const dt_entry_count = buf.readUInt32BE(16);
  const dt_entries_offset = buf.readUInt32BE(20);
  const page_size = buf.readUInt32BE(24);
  const version = buf.readUInt32BE(28);

  const entries = [];
  for (let i = 0; i < dt_entry_count; i++) {
    const base = dt_entries_offset + i * dt_entry_size;
    if (base + 32 > buf.length) break;
    entries.push({
      index: i,
      dt_size: buf.readUInt32BE(base),
      dt_offset: buf.readUInt32BE(base + 4),
      id: buf.readUInt32BE(base + 8),
      rev: buf.readUInt32BE(base + 12),
    });
  }
  return { kind: 'dtbo', total_size, header_size, dt_entry_size, dt_entry_count, page_size, version, entries };
}

// ---------- CLI ----------
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [mode, inp, outdir] = process.argv.slice(2);
  if (!mode || !inp) {
    console.error('usage: node bootimg.mjs <boot|vendorboot|dtbo> <image> [outdir]');
    process.exit(2);
  }
  const buf = readFileSync(inp);
  let info;
  if (mode === 'boot') info = parseBoot(buf);
  else if (mode === 'vendorboot') info = parseVendorBoot(buf);
  else if (mode === 'dtbo') info = parseDtbo(buf);
  else { console.error('mode must be boot|vendorboot|dtbo'); process.exit(2); }

  if (outdir) {
    mkdirSync(outdir, { recursive: true });
    if (info.parts) {
      for (const p of info.parts) {
        const dst = join(outdir, p.name + '.bin');
        writeFileSync(dst, buf.subarray(p.offset, p.offset + p.size));
        console.log(`extracted ${p.name}: ${p.size} bytes -> ${dst}`);
      }
    }
    if (info.entries) {
      for (const e of info.entries) {
        const dst = join(outdir, `dtbo_${String(e.index).padStart(2, '0')}_id${e.id}_rev${e.rev}.dtb`);
        writeFileSync(dst, buf.subarray(e.dt_offset, e.dt_offset + e.dt_size));
        console.log(`extracted entry ${e.index}: ${e.dt_size} bytes (id=${e.id} rev=${e.rev}) -> ${dst}`);
      }
    }
  }
  const { parts, entries, ...rest } = info;
  console.log(JSON.stringify(rest, (k, v) => typeof v === 'bigint' ? v.toString() : v, 2));
}
