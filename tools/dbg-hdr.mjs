#!/usr/bin/env node
import { readFileSync } from 'node:fs';
import { parseFdt } from './fdt.mjs';

const path = process.argv[2];
const buf = readFileSync(path);
console.log('file:', path, 'size:', buf.length);
console.log('magic: 0x' + buf.readUInt32BE(0).toString(16));
console.log('totalsize:', buf.readUInt32BE(4));
console.log('offStruct:', '0x' + buf.readUInt32BE(8).toString(16));
console.log('offStrings:', '0x' + buf.readUInt32BE(12).toString(16));
console.log('offRsvmap:', '0x' + buf.readUInt32BE(16).toString(16));
console.log('version:', buf.readUInt32BE(20), 'lastComp:', buf.readUInt32BE(24));
console.log('sizeStrings:', buf.readUInt32BE(32), 'sizeStruct:', buf.readUInt32BE(36));

const t = parseFdt(buf);
console.log('parsed root name:', JSON.stringify(t.root.name));
console.log('root props:', t.root.props.length, 'children:', t.root.children.length);
console.log('children names:', t.root.children.map(c => JSON.stringify(c.name)).slice(0, 20).join(' '));
