#!/usr/bin/env node
// SCS keeps its pointer in x18, the register ARM64EC needs for the TEB.
import { readFileSync } from 'node:fs';

const SCS_PUSH = 0xf8008e52; // str x18, [x18], #8
const SCS_POP_PRE = 0xf81f8e52; // ldr x18, [x18, #-8]!
const SCS_POP_POST = 0xf81f8652; // ldr x18, [x18], #-8

const PT_LOAD = 1;
const PF_X = 1;
const EM_AARCH64 = 183;

function scanBuffer(buf, start, end) {
  const hits = { push: 0, pop: 0 };
  for (let off = start; off + 4 <= end; off += 4) {
    const w = buf.readUInt32LE(off);
    if (w === SCS_PUSH) hits.push++;
    else if (w === SCS_POP_PRE || w === SCS_POP_POST) hits.pop++;
  }
  return hits;
}

function execSegments(buf) {
  if (buf.length < 64) throw new Error('shorter than an ELF64 header');
  if (buf.readUInt32BE(0) !== 0x7f454c46) throw new Error('not an ELF file');
  if (buf[4] !== 2) throw new Error('not ELF64');
  if (buf[5] !== 1) throw new Error('not little-endian');

  const machine = buf.readUInt16LE(0x12);
  const phoff = Number(buf.readBigUInt64LE(0x20));
  const phentsize = buf.readUInt16LE(0x36);
  const phnum = buf.readUInt16LE(0x38);
  if (phentsize < 56) throw new Error(`unexpected e_phentsize ${phentsize}`);

  const segs = [];
  for (let i = 0; i < phnum; i++) {
    const p = phoff + i * phentsize;
    if (p + 56 > buf.length) break;
    const type = buf.readUInt32LE(p);
    const flags = buf.readUInt32LE(p + 4);
    const offset = Number(buf.readBigUInt64LE(p + 8));
    const vaddr = Number(buf.readBigUInt64LE(p + 16));
    const filesz = Number(buf.readBigUInt64LE(p + 32));
    if (type !== PT_LOAD || !(flags & PF_X) || filesz === 0) continue;
    if (offset + filesz > buf.length) continue;
    segs.push({ offset, vaddr, filesz });
  }
  return { machine, segs };
}

function scanFile(path) {
  const buf = readFileSync(path);
  const { machine, segs } = execSegments(buf);
  let push = 0;
  let pop = 0;
  let bytes = 0;
  for (const s of segs) {
    const hits = scanBuffer(buf, s.offset, s.offset + s.filesz);
    push += hits.push;
    pop += hits.pop;
    bytes += s.filesz;
  }
  return {
    file: path,
    machine: machine === EM_AARCH64 ? 'aarch64' : `elf-machine-${machine}`,
    exec_segments: segs.length,
    exec_bytes: bytes,
    scs_push: push,
    scs_pop: pop,
    per_mib: bytes > 0 ? Number(((push / bytes) * 1048576).toFixed(1)) : 0,
  };
}

function selfTest() {
  const buf = Buffer.alloc(4096);
  const words = [SCS_PUSH, SCS_POP_PRE, SCS_POP_POST, SCS_PUSH, 0xd503201f];
  words.forEach((w, i) => buf.writeUInt32LE(w, 64 + i * 4));
  const hits = scanBuffer(buf, 0, buf.length);
  const ok = hits.push === 2 && hits.pop === 2;
  console.log(JSON.stringify({ self_test: ok ? 'ok' : 'failed', ...hits }));
  return ok;
}

const args = process.argv.slice(2);
if (args.length === 0) {
  console.error('usage: dso_x18_scan.mjs [--self-test] [--expect-zero] <elf>...');
  process.exit(2);
}
if (args.includes('--self-test')) process.exit(selfTest() ? 0 : 1);

const expectZero = args.includes('--expect-zero');
const files = args.filter((a) => !a.startsWith('--'));
let dirty = 0;

for (const file of files) {
  let result;
  try {
    result = scanFile(file);
  } catch (err) {
    console.error(`${file}: ${err.message}`);
    process.exit(2);
  }
  console.log(JSON.stringify(result));
  if (result.scs_push > 0 || result.scs_pop > 0) dirty++;
}

if (expectZero && dirty > 0) {
  console.error(`${dirty} file(s) contain SCS x18 sequences`);
  process.exit(1);
}
