#!/usr/bin/env node

const fs = require('fs');
const path = require('path');

const assets = process.argv[2] || path.join(process.cwd(), 'orig', 'GALE01', 'files');

const extSizes = {
  ftDataBoy: 0x004, ftDataCaptain: 0x08c, ftDataCrazyhand: 0x144,
  ftDataClink: 0x0dc, ftDataDonkey: 0x074, ftDataDrmario: 0x084,
  ftDataFalco: 0x0d4, ftDataEmblem: 0x098, ftDataFox: 0x0d4,
  ftDataGkoopa: 0x0a0, ftDataGirl: 0x004, ftDataGanon: 0x08c,
  ftDataGamewatch: 0x094, ftDataKirby: 0x424, ftDataKoopa: 0x0a0,
  ftDataLuigi: 0x098, ftDataLink: 0x0dc, ftDataMasterhand: 0x17c,
  ftDataMario: 0x084, ftDataMars: 0x098, ftDataMewtwo: 0x088,
  ftDataNana: 0x15c, ftDataNess: 0x0dc, ftDataPichu: 0x0f8,
  ftDataPeach: 0x0c0, ftDataPikachu: 0x0f8, ftDataPopo: 0x15c,
  ftDataPurin: 0x100, ftDataSandbag: 0x008, ftDataSeak: 0x074,
  ftDataSamus: 0x0d4, ftDataYoshi: 0x138, ftDataZelda: 0x0a8,
};

function die(message) {
  throw new Error(message);
}

function parseDat(file) {
  const b = fs.readFileSync(file);
  if (b.length < 32 || b.readUInt32BE(0) !== b.length) die(`${file}: bad DAT header`);
  const dataSize = b.readUInt32BE(4);
  const relocCount = b.readUInt32BE(8);
  const publicCount = b.readUInt32BE(12);
  const externCount = b.readUInt32BE(16);
  const relocOff = 32 + dataSize;
  const publicOff = relocOff + relocCount * 4;
  const stringsOff = publicOff + (publicCount + externCount) * 8;
  if (stringsOff > b.length) die(`${file}: tables outside file`);

  const relocs = [];
  const pointerFields = new Set();
  for (let i = 0; i < relocCount; ++i) {
    const field = b.readUInt32BE(relocOff + i * 4);
    if (field + 4 > dataSize) die(`${file}: relocation outside data`);
    relocs.push(field);
    pointerFields.add(field);
  }

  const publics = [];
  for (let i = 0; i < publicCount; ++i) {
    const off = publicOff + i * 8;
    const target = b.readUInt32BE(off);
    const nameOff = b.readUInt32BE(off + 4);
    const start = stringsOff + nameOff;
    const end = b.indexOf(0, start);
    if (target > dataSize || start < stringsOff || end < start) die(`${file}: bad public`);
    publics.push({ name: b.toString('ascii', start, end), target });
  }

  const targets = [...new Set([
    ...relocs.map((field) => b.readUInt32BE(32 + field)),
    ...publics.map((entry) => entry.target),
  ].filter((target) => target >= 0 && target < dataSize))].sort((a, c) => a - c);

  function word(off) {
    if (!Number.isInteger(off) || off < 0 || off + 4 > dataSize) die(`${file}: bad word ${off}`);
    return b.readUInt32BE(32 + off);
  }
  function half(off) {
    if (!Number.isInteger(off) || off < 0 || off + 2 > dataSize) die(`${file}: bad half ${off}`);
    return b.readUInt16BE(32 + off);
  }
  function span(target) {
    if (!Number.isInteger(target) || target < 0 || target > dataSize) die(`${file}: bad target ${target}`);
    let next = dataSize;
    for (const candidate of targets) if (candidate > target && candidate < next) next = candidate;
    return next - target;
  }
  function pointer(field, allowNull = true) {
    const value = word(field);
    if (pointerFields.has(field)) return value;
    if (allowNull && value === 0) return null;
    die(`${file}: expected relocation/null at 0x${field.toString(16)}`);
  }
  function requireSpan(target, bytes, what) {
    if (target === null || target + bytes > dataSize) die(`${file}: ${what} outside data`);
  }
  return { file, b, dataSize, publics, pointerFields, word, half, span, pointer, requireSpan };
}

function auditMotion(dat, table, count, maxSize, ajSize, label) {
  dat.requireSpan(table, count * 0x18, label);
  for (let i = 0; i < count; ++i) {
    const off = table + i * 0x18;
    dat.pointer(off + 0x00);
    dat.pointer(off + 0x0c);
    const ajOff = dat.word(off + 0x04);
    const bytes = dat.word(off + 0x08);
    if (bytes > maxSize) die(`${dat.file}: ${label}[${i}] size 0x${bytes.toString(16)}`);
    if (ajSize !== null && ajOff + bytes > ajSize) {
      die(`${dat.file}: ${label}[${i}] AJ range ${ajOff}+${bytes} > ${ajSize}`);
    }
  }
}

function assertNoRelocations(dat, start, bytes, label, skip = new Set()) {
  dat.requireSpan(start, bytes, label);
  for (let off = 0; off < bytes; off += 4) {
    if (!skip.has(off) && dat.pointerFields.has(start + off)) {
      die(`${dat.file}: ${label} scalar range contains relocation at +0x${off.toString(16)}`);
    }
  }
}

function auditAnimJointTree(dat, root, label) {
  const seen = new Set();
  function walk(off) {
    if (off === null || seen.has(off)) return;
    seen.add(off);
    dat.requireSpan(off, 0x14, label);
    const child = dat.pointer(off + 0x00);
    const next = dat.pointer(off + 0x04);
    const aobj = dat.pointer(off + 0x08);
    const robj = dat.pointer(off + 0x0c);
    if (aobj !== null) dat.requireSpan(aobj, 0x10, `${label} AObjDesc`);
    if (robj !== null) dat.requireSpan(robj, 8, `${label} RObjAnimJoint`);
    walk(child);
    walk(next);
  }
  walk(root);
  return seen.size;
}

function auditColorCommandGraphs(dat, roots, label) {
  const lengths = new Map([
    [0, 1], [1, 1], [2, 1], [3, 1], [4, 1], [5, 2], [6, 1], [7, 2],
    [8, 1], [9, 1], [10, 1], [11, 1], [12, 1], [13, 2], [14, 2],
    [15, 2], [16, 1], [17, 1], [18, 2], [19, 2], [20, 1], [21, 5],
    [22, 3], [23, 1],
  ]);
  const seen = new Set();
  const histogram = new Map();
  function walk(start) {
    let off = start;
    while (off !== null && !seen.has(off)) {
      dat.requireSpan(off, 4, `${label} command`);
      seen.add(off);
      const raw = dat.word(off);
      const opcode = raw >>> 26;
      const words = lengths.get(opcode);
      if (words === undefined) die(`${dat.file}: unsupported ${label} opcode ${opcode} at 0x${off.toString(16)}`);
      dat.requireSpan(off, words * 4, `${label} command payload`);
      histogram.set(opcode, (histogram.get(opcode) || 0) + 1);
      if (opcode === 5 || opcode === 7) {
        const target = dat.pointer(off + 4, false);
        walk(target);
        if (opcode === 7) return;
      }
      if (opcode === 23 && (raw & 0x1ffff) !== 0) {
        die(`${dat.file}: nonzero padding in ${label} opcode 23 at 0x${off.toString(16)}`);
      }
      if (opcode === 0 || opcode === 6 || opcode === 10) return;
      off += words * 4;
    }
  }
  for (const root of roots) walk(root);
  return { commands: seen.size, histogram };
}

function auditFighterPartsHsd(dat, root) {
  if (root === null) return { present: 0, joints: 0, dobjs: 0, mobjs: 0, pobjs: 0, tobjs: 0, robjs: 0 };
  const joints = new Set(), dobjs = new Set(), mobjs = new Set();
  const pobjs = new Set(), tobjs = new Set(), vtx = new Set();
  const images = new Set(), tluts = new Set(), envelopes = new Set();
  const robjs = new Set(), bcexps = new Set(), rvalueLists = new Set();
  const robjJointRefs = new Set();

  function auditVtx(start) {
    if (vtx.has(start)) return;
    vtx.add(start);
    for (let i = 0; i < 32; ++i) {
      const off = start + i * 0x18;
      dat.requireSpan(off, 0x18, 'fighter x5C vtxdesc');
      const attr = dat.word(off);
      if (attr === 0xff) {
        if (i === 0) die(`${dat.file}: empty fighter x5C vtxdesc`);
        return;
      }
      const attrType = dat.word(off + 4);
      const vertex = dat.pointer(off + 0x14);
      if (attrType !== 1 && vertex === null) {
        die(`${dat.file}: indexed fighter x5C vtxdesc without vertex data`);
      }
    }
    die(`${dat.file}: unterminated fighter x5C vtxdesc`);
  }

  function auditTObj(off) {
    if (tobjs.has(off)) return;
    tobjs.add(off);
    dat.requireSpan(off, 0x5c, 'fighter x5C TObj');
    const next = dat.pointer(off + 4);
    if (next !== null) auditTObj(next);
    const image = dat.pointer(off + 0x4c, false);
    if (!images.has(image)) {
      images.add(image);
      dat.requireSpan(image, 0x18, 'fighter x5C image');
      dat.pointer(image, false);
    }
    const tlut = dat.pointer(off + 0x50);
    if (tlut !== null && !tluts.has(tlut)) {
      tluts.add(tlut);
      dat.requireSpan(tlut, 0x10, 'fighter x5C tlut');
      dat.pointer(tlut, false);
    }
    const lod = dat.pointer(off + 0x54);
    if (lod !== null) dat.requireSpan(lod, 0x10, 'fighter x5C lod');
    const tev = dat.pointer(off + 0x58);
    if (tev !== null) dat.requireSpan(tev, 0x20, 'fighter x5C tev');
  }

  function auditEnvelope(array) {
    if (envelopes.has(array)) return;
    envelopes.add(array);
    for (let matrix = 0; matrix < 32; ++matrix) {
      const desc = dat.pointer(array + matrix * 4);
      if (desc === null) return;
      let terminated = false;
      for (let weight = 0; weight < 32; ++weight) {
        const entry = desc + weight * 8;
        dat.requireSpan(entry, 8, 'fighter x5C envelope weight');
        const joint = dat.pointer(entry);
        if (joint === null) {
          terminated = true;
          break;
        }
        auditJoint(joint);
      }
      if (!terminated) die(`${dat.file}: fighter x5C envelope weights unterminated`);
    }
    die(`${dat.file}: fighter x5C envelope matrices unterminated`);
  }

  function auditPObj(off) {
    if (pobjs.has(off)) return;
    pobjs.add(off);
    dat.requireSpan(off, 0x18, 'fighter x5C PObj');
    const next = dat.pointer(off + 4);
    if (next !== null) auditPObj(next);
    auditVtx(dat.pointer(off + 8, false));
    const flags = dat.half(off + 0x0c);
    const type = flags & 0x3000;
    if (type === 0x1000 || type === 0x3000) {
      die(`${dat.file}: unsupported fighter x5C PObj flags 0x${flags.toString(16)}`);
    }
    dat.pointer(off + 0x10, false);
    const union = dat.pointer(off + 0x14);
    if (type === 0x2000) {
      if (union === null) die(`${dat.file}: fighter x5C envelope PObj without data`);
      auditEnvelope(union);
    } else if (union !== null) {
      auditJoint(union);
    }
  }

  function auditMObj(off) {
    if (mobjs.has(off)) return;
    mobjs.add(off);
    dat.requireSpan(off, 0x18, 'fighter x5C MObj');
    const mode = dat.word(off + 4);
    if ((mode & 0x60000000) === 0x20000000) {
      die(`${dat.file}: invalid fighter x5C MObj blending 0x${mode.toString(16)}`);
    }
    const tex = dat.pointer(off + 8);
    if (tex !== null) auditTObj(tex);
    const material = dat.pointer(off + 0x0c, false);
    dat.requireSpan(material, 0x14, 'fighter x5C material');
    if (dat.pointer(off + 0x10) !== null) {
      die(`${dat.file}: fighter x5C MObj renderdesc requires adapter`);
    }
    const pe = dat.pointer(off + 0x14);
    if (pe !== null) dat.requireSpan(pe, 0x0c, 'fighter x5C PEDesc');
  }

  function auditDObj(off) {
    if (dobjs.has(off)) return;
    dobjs.add(off);
    dat.requireSpan(off, 0x10, 'fighter x5C DObj');
    const next = dat.pointer(off + 4);
    if (next !== null) auditDObj(next);
    const mobj = dat.pointer(off + 8);
    if (mobj !== null) auditMObj(mobj);
    const pobj = dat.pointer(off + 0x0c);
    if (pobj !== null) auditPObj(pobj);
  }

  function auditSpline(off) {
    dat.requireSpan(off, 0x18, 'fighter x5C spline');
    const type = dat.b[32 + off];
    const numcv = dat.b.readInt16BE(32 + off + 2);
    if (type > 3 || numcv < 2 || numcv > 4096) {
      die(`${dat.file}: invalid fighter x5C spline header`);
    }
    dat.pointer(off + 8, false);
    dat.pointer(off + 0x10, false);
    dat.pointer(off + 0x14, false);
  }

  function auditRvalueList(off) {
    if (rvalueLists.has(off)) return;
    rvalueLists.add(off);
    for (let i = 0; i < 256; ++i) {
      const entry = off + i * 8;
      dat.requireSpan(entry, 8, 'HSD RvalueList');
      const joint = dat.pointer(entry + 4);
      if (joint === null) return;
      robjJointRefs.add(joint);
    }
    die(`${dat.file}: HSD RvalueList unterminated`);
  }

  function auditBcExp(off) {
    if (bcexps.has(off)) return;
    bcexps.add(off);
    dat.requireSpan(off, 8, 'HSD ByteCodeExpDesc');
    dat.pointer(off);
    const rvalue = dat.pointer(off + 4);
    if (rvalue !== null) auditRvalueList(rvalue);
  }

  function auditRObj(off) {
    while (off !== null && !robjs.has(off)) {
      robjs.add(off);
      dat.requireSpan(off, 12, 'HSD RObjDesc');
      const next = dat.pointer(off);
      const flags = dat.word(off + 4);
      const type = flags & 0x70000000;
      const payload = dat.pointer(off + 8);
      if (type !== 0x30000000 || payload === null) {
        die(`${dat.file}: unsupported HSD RObj flags 0x${flags.toString(16)}`);
      }
      auditBcExp(payload);
      off = next;
    }
  }

  function auditJoint(off) {
    if (joints.has(off)) return;
    joints.add(off);
    dat.requireSpan(off, 0x40, 'fighter x5C JObj');
    const flags = dat.word(off + 4);
    const unsupported = flags & (0x20 | 0x1000 | 0x20000 | 0x600000 | 0x800000);
    const billboard = flags & 0xe00;
    if (unsupported || (billboard && ![0x200, 0x400, 0x600, 0x800].includes(billboard))) {
      die(`${dat.file}: unsupported fighter x5C JObj flags 0x${flags.toString(16)}`);
    }
    const child = dat.pointer(off + 8);
    const next = dat.pointer(off + 0x0c);
    if (child !== null) auditJoint(child);
    if (next !== null) auditJoint(next);
    const union = dat.pointer(off + 0x10);
    if (flags & 0x4000) {
      if (union === null) die(`${dat.file}: fighter x5C spline JObj without spline`);
      auditSpline(union);
    } else if (union !== null) {
      auditDObj(union);
    }
    dat.pointer(off + 0x38);
    const robj = dat.pointer(off + 0x3c);
    if (robj !== null) auditRObj(robj);
  }

  auditJoint(root);
  for (const joint of robjJointRefs) {
    if (!joints.has(joint)) {
      die(`${dat.file}: RObj references JObj outside structural tree 0x${joint.toString(16)}`);
    }
  }
  return {
    present: 1,
    joints: joints.size,
    dobjs: dobjs.size,
    mobjs: mobjs.size,
    pobjs: pobjs.size,
    tobjs: tobjs.size,
    robjs: robjs.size,
  };
}

function auditFighter(file) {
  const dat = parseDat(file);
  const roots = dat.publics.filter((entry) => entry.name.startsWith('ftData'));
  if (roots.length !== 1) die(`${file}: expected one ftData root, got ${roots.length}`);
  const { name, target: root } = roots[0];
  if (!(name in extSizes)) die(`${file}: missing ext schema for ${name}`);
  dat.requireSpan(root, 0x60, 'ftData');
  const fields = Array.from({ length: 24 }, (_, i) => dat.pointer(root + i * 4));
  for (const required of [0, 1, 2, 3, 5]) if (fields[required] === null) die(`${file}: missing root field ${required}`);

  dat.requireSpan(fields[0], 0x184, 'ftCo_DatAttrs');
  assertNoRelocations(dat, fields[0], 0x180, 'ftCo_DatAttrs');
  if (dat.span(fields[1]) < extSizes[name]) die(`${file}: short ext_attr for ${name}`);

  dat.requireSpan(fields[2], 0x15, 'ftData_x8');
  dat.pointer(fields[2] + 4);
  const costumeTobjCount = dat.word(fields[2] + 8);
  const costumeTobjTable = dat.pointer(fields[2] + 0x0c, false);
  if (costumeTobjCount > 5 || costumeTobjTable === null) {
    die(`${file}: invalid costume TObj count/table ${costumeTobjCount}`);
  }
  const costumeTobjTableBytes = dat.span(costumeTobjTable);
  if (!costumeTobjTableBytes || costumeTobjTableBytes > 0x18 || costumeTobjTableBytes % 4) {
    die(`${file}: invalid costume TObj pointer-table span 0x${costumeTobjTableBytes.toString(16)}`);
  }

  const modelNum = dat.word(fields[2]);
  const visTable = dat.pointer(fields[2] + 4, false);
  if (modelNum > 11 || visTable === null) die(`${file}: invalid fighter visibility root`);
  const visTableBytes = dat.span(visTable);
  if (!visTableBytes || visTableBytes > 0x80 || visTableBytes % 0x10) {
    die(`${file}: invalid visibility table span 0x${visTableBytes.toString(16)}`);
  }
  const visLookups = new Set();
  const visTemps = new Set();
  let visTempEntries = 0;
  let visByteIndices = 0;
  for (let cell = 0; cell < visTableBytes / 4; ++cell) {
    const lookup = dat.pointer(visTable + cell * 4);
    if (lookup === null || visLookups.has(lookup)) continue;
    visLookups.add(lookup);
    dat.requireSpan(lookup, modelNum * 8, 'FtPartsVisLookup[]');
    for (let model = 0; model < modelNum; ++model) {
      const rec = lookup + model * 8;
      const count = dat.word(rec);
      const temp = dat.pointer(rec + 4);
      if (count > 32 || (count && temp === null)) {
        die(`${file}: invalid FtPartsVisLookup count ${count}`);
      }
      if (temp === null || visTemps.has(temp)) continue;
      visTemps.add(temp);
      dat.requireSpan(temp, count * 8, 'TempS[]');
      for (let j = 0; j < count; ++j) {
        const entry = temp + j * 8;
        const indexCount = dat.word(entry);
        const indices = dat.pointer(entry + 4);
        if (indexCount > 256 || (indexCount && indices === null)) {
          die(`${file}: invalid TempS count ${indexCount}`);
        }
        if (indexCount) dat.requireSpan(indices, indexCount, 'TempS byte indices');
        visTempEntries++;
        visByteIndices += indexCount;
      }
    }
  }

  const costumeTobjArrays = new Set();
  let costumeTobjIndices = 0;
  for (let i = 0; i < costumeTobjTableBytes / 4; ++i) {
    const indices = dat.pointer(costumeTobjTable + i * 4);
    if (indices === null || costumeTobjArrays.has(indices)) continue;
    costumeTobjArrays.add(indices);
    dat.requireSpan(indices, costumeTobjCount * 2, 'costume TObj indices');
    for (let j = 0; j < costumeTobjCount; ++j) {
      const index = dat.half(indices + j * 2);
      if (index > 0xff) die(`${file}: costume TObj index ${index} > 255`);
      costumeTobjIndices++;
    }
  }

  const mainBytes = fields[5] - fields[3];
  if (mainBytes <= 0 || mainBytes % 0x18) die(`${file}: invalid main motion extent`);
  const motionCount = mainBytes / 0x18;
  const demoBytes = dat.span(fields[5]);
  if (!demoBytes || demoBytes % 0x18) die(`${file}: invalid demo motion extent`);

  const code = path.basename(file).slice(2, 4);
  const aj = path.join(path.dirname(file), `Pl${code}AJ.dat`);
  if (!fs.existsSync(aj)) die(`${file}: missing ${path.basename(aj)}`);
  const ajSize = fs.statSync(aj).size;
  auditMotion(dat, fields[3], motionCount, 0x8000, ajSize, 'motion');
  auditMotion(dat, fields[5], demoBytes / 0x18, 0xb000, null, 'demo-motion');

  if (fields[7] !== null) {
    const bytes = dat.span(fields[7]);
    if (!bytes || bytes > 0x100 || bytes % 4) die(`${file}: invalid x1C pointer table`);
    for (let i = 0; i < bytes / 4; ++i) {
      const rec = dat.pointer(fields[7] + i * 4);
      if (rec === null) continue;
      dat.requireSpan(rec, 0x0c, 'x1C record');
      dat.pointer(rec + 4);
      dat.pointer(rec + 8);
    }
  }

  for (const [field, align, max, label] of [
    [9, 8, 0x400, 'x24'], [10, 8, 0x400, 'x28'],
    [13, 8, 0x200, 'x34'], [14, 0x14, 0x400, 'x38'],
    [21, 4, 0x100, 'x54'],
  ]) {
    if (fields[field] === null) continue;
    const bytes = dat.span(fields[field]);
    if (!bytes || bytes > max || bytes % align) die(`${file}: invalid ${label} span 0x${bytes.toString(16)}`);
    assertNoRelocations(dat, fields[field], bytes, label);
  }

  if (fields[11] !== null) {
    const dyn = fields[11];
    dat.requireSpan(dyn, 0x14, 'ftDynamics');
    const n = dat.word(dyn);
    const hitCount = dat.word(dyn + 8);
    const bones = dat.pointer(dyn + 4);
    const hits = dat.pointer(dyn + 0x0c);
    dat.pointer(dyn + 0x10);
    if (n >= 10 || hitCount > 11) die(`${file}: invalid dynamics counts ${n}/${hitCount}`);
    if (n && bones === null) die(`${file}: missing dynamics bones`);
    if (hitCount && hits === null) die(`${file}: missing dynamics hits`);
    const sources = new Map();
    for (let i = 0; i < n; ++i) {
      const rec = bones + i * 0x18;
      dat.requireSpan(rec, 0x18, 'BoneDynamicsDesc');
      const source = dat.pointer(rec + 4);
      const count = dat.word(rec + 8);
      if (count > 32 || (count && source === null)) die(`${file}: invalid BoneDynamicsDesc ${i}`);
      if (source !== null) sources.set(source, Math.max(sources.get(source) || 0, count));
    }
    for (const [source, count] of sources) {
      dat.requireSpan(source, count * 0x3c, 'dynamics source');
      assertNoRelocations(dat, source, count * 0x3c, 'dynamics source');
    }
    if (hitCount) {
      dat.requireSpan(hits, hitCount * 0x14, 'dynamics collision table');
      assertNoRelocations(dat, hits, hitCount * 0x14, 'dynamics collision table');
    }
  }

  if (fields[12] !== null) {
    const n = dat.word(fields[12]);
    const list = dat.pointer(fields[12] + 4);
    if (n > 15 || (n && list === null)) die(`${file}: invalid hurtbox table`);
    if (n) dat.requireSpan(list, n * 0x28, 'hurtboxes');
  }

  if (fields[19] !== null) {
    assertNoRelocations(dat, fields[19] + 0x04, 0x18, 'FtSFX scalar 04..18');
    assertNoRelocations(dat, fields[19] + 0x24, 0x14, 'FtSFX scalar 24..34');
    for (const wordOff of [0, 0x1c, 0x20]) {
      const arr = dat.pointer(fields[19] + wordOff);
      if (arr === null) continue;
      dat.requireSpan(arr, 8, 'FtSFXArr');
      const n = dat.word(arr);
      const ids = dat.pointer(arr + 4);
      if (n > 2048 || (n && ids === null)) die(`${file}: invalid FtSFXArr count`);
      if (n) {
        dat.requireSpan(ids, n * 4, 'FtSFX ids');
        assertNoRelocations(dat, ids, n * 4, 'FtSFX ids');
      }
    }
  }
  const partsHsd = auditFighterPartsHsd(dat, fields[23]);
  return {
    name,
    motionCount,
    demoCount: demoBytes / 0x18,
    costumeTobjArrays: costumeTobjArrays.size,
    costumeTobjIndices,
    visLookups: visLookups.size,
    visTemps: visTemps.size,
    visTempEntries,
    visByteIndices,
    partsHsd,
  };
}

function publicTarget(dat, wanted) {
  const matches = dat.publics.filter((entry) => entry.name === wanted);
  if (matches.length !== 1) die(`${dat.file}: expected ${wanted}`);
  return matches[0].target;
}

function auditPlCo(file) {
  const dat = parseDat(file);
  const root = publicTarget(dat, 'ftLoadCommonData');
  dat.requireSpan(root, 23 * 4, 'ftLoadCommonData');
  const p = Array.from({ length: 23 }, (_, i) => dat.pointer(root + i * 4));
  if (p.some((value) => value === null)) die(`${file}: null common-data root pointer`);
  dat.requireSpan(p[0], 0x818, 'ftCommonData');
  if (dat.span(p[0]) !== 0x818) die(`${file}: unexpected ftCommonData span`);
  if (dat.span(p[2]) < 0x78 || dat.span(p[3]) < 0x24 || dat.span(p[12]) < 0x9c || dat.span(p[21]) < 0x44) {
    die(`${file}: common scalar table too short`);
  }
  const partBytes = dat.span(p[4]);
  if (!partBytes || partBytes > 0x200 || partBytes % 4) die(`${file}: bad parts table`);
  for (let i = 0; i < partBytes / 4; ++i) {
    const rec = dat.pointer(p[4] + i * 4);
    if (rec === null) continue;
    dat.requireSpan(rec, 12, 'FighterPartsTable');
    dat.pointer(rec);
    dat.pointer(rec + 4);
    if (dat.word(rec + 8) > 255) die(`${file}: parts_num > 255`);
  }

  if (dat.span(p[1]) !== 0x138) die(`${file}: unexpected item-throw attr span`);
  assertNoRelocations(dat, p[1], 0x138, 'item-throw attrs');

  const specialPartsBytes = dat.span(p[5]);
  if (specialPartsBytes !== 34 * 4) die(`${file}: unexpected special-parts table span`);
  let specialPartsRecords = 0;
  let specialPartsEntries = 0;
  for (let i = 0; i < 34; ++i) {
    const rec = dat.pointer(p[5] + i * 4);
    if (rec === null) continue;
    specialPartsRecords++;
    dat.requireSpan(rec, 8, 'special-parts record');
    const entries = dat.pointer(rec);
    const count = dat.word(rec + 4);
    if (count > 32 || (count && entries === null)) {
      die(`${file}: invalid special-parts count ${count} for fighter ${i}`);
    }
    if (count) dat.requireSpan(entries, count * 4, 'special-parts byte records');
    specialPartsEntries += count;
  }

  if (dat.span(p[9]) !== 0x18) die(`${file}: unexpected model-shift table span`);
  let modelShiftVectors = 0;
  for (let i = 0; i < 3; ++i) {
    const vectors = dat.pointer(p[9] + i * 8);
    const count = dat.word(p[9] + i * 8 + 4);
    if (count > 64 || (count && vectors === null)) {
      die(`${file}: invalid model-shift vector count ${count}`);
    }
    if (count) {
      dat.requireSpan(vectors, count * 8, 'model-shift Vec2[]');
      assertNoRelocations(dat, vectors, count * 8, 'model-shift Vec2[]');
      modelShiftVectors += count;
    }
  }

  for (const [idx, bytes, label] of [
    [13, 0x3c, 'bunnyhood modifiers'],
    [14, 0x24, 'metal modifiers'],
    [15, 0x08, 'gravity/weight modifiers'],
  ]) {
    if (dat.span(p[idx]) !== bytes) die(`${file}: unexpected ${label} span`);
    assertNoRelocations(dat, p[idx], bytes, label);
  }

  if (dat.span(p[8]) !== 8) die(`${file}: unexpected common-model root table span`);
  const commonModel0 = dat.pointer(p[8], false);
  const commonAnim = dat.pointer(p[8] + 4, false);
  const commonModels = [
    auditFighterPartsHsd(dat, commonModel0),
    auditFighterPartsHsd(dat, p[16]),
    auditFighterPartsHsd(dat, p[20]),
  ];
  const commonAnimJoints = auditAnimJointTree(dat, commonAnim, 'common accessory AnimJoint');

  const colorRoots = [];
  for (const [idx, expectedSpan] of [[6, 0x3d8], [7, 0x30]]) {
    if (dat.span(p[idx]) !== expectedSpan) die(`${file}: unexpected color-table ${idx} span`);
    for (let off = 0; off < expectedSpan; off += 8) {
      const script = dat.pointer(p[idx] + off);
      if (script !== null) colorRoots.push(script);
    }
  }
  const colorCommands = auditColorCommandGraphs(dat, colorRoots, 'ColorOverlay');

  const aiRoot = p[22];
  if (dat.span(aiRoot) !== 0x30) die(`${file}: unexpected CPU common root span`);
  const ai = Array.from({ length: 10 }, (_, i) => dat.pointer(aiRoot + i * 4, false));
  if (dat.span(ai[0]) !== 0xf8) die(`${file}: unexpected CPU cmdscript pointer-table span`);
  for (let off = 0; off < 0xf8; off += 4) dat.pointer(ai[0] + off);
  let cpuAttackLists = 0;
  let cpuAttackEntries = 0;
  const seenAttackLists = new Set();
  for (let field = 1; field <= 7; ++field) {
    if (dat.span(ai[field]) !== 0x80) die(`${file}: unexpected CPU attack pointer-table span ${field}`);
    for (let i = 0; i < 32; ++i) {
      const list = dat.pointer(ai[field] + i * 4);
      if (list === null || seenAttackLists.has(list)) continue;
      seenAttackLists.add(list);
      cpuAttackLists++;
      let count = 0;
      for (; count < 64; ++count) {
        const rec = list + count * 0x24;
        dat.requireSpan(rec, 0x24, 'CPU attack record');
        assertNoRelocations(dat, rec, 0x24, 'CPU attack record');
        if (dat.word(rec) === 0) break;
      }
      if (count === 64) die(`${file}: unterminated CPU attack list at 0x${list.toString(16)}`);
      cpuAttackEntries += count;
    }
  }
  if (dat.span(ai[8]) !== 0x80 || dat.span(ai[9]) !== 0x18) {
    die(`${file}: unexpected CPU float-table spans`);
  }
  assertNoRelocations(dat, ai[8], 0x80, 'CPU fighter reach');
  assertNoRelocations(dat, ai[9], 0x18, 'CPU weapon reach');

  return {
    specialPartsRecords,
    specialPartsEntries,
    modelShiftVectors,
    commonModels,
    commonAnimJoints,
    colorCommands,
    cpuAttackLists,
    cpuAttackEntries,
  };
}

function auditItCo(file) {
  const dat = parseDat(file);
  const root = publicTarget(dat, 'itPublicData');
  dat.requireSpan(root, 0x18, 'itPublicData');
  const p = Array.from({ length: 6 }, (_, i) => dat.pointer(root + i * 4));
  if (p[0] === null) die(`${file}: missing ItemCommonData`);
  dat.requireSpan(p[0], 0x160, 'ItemCommonData');
  if (dat.span(p[0]) < 0x160) die(`${file}: short ItemCommonData`);

  const articleCounts = [43, 118, 47];
  const articles = new Set();
  const attrs = new Set();
  const hurts = new Set();
  const models = new Set();
  const dynamics = new Set();
  const dynamicsSources = new Set();
  let modelHsdRoots = 0;
  let modelHsdJoints = 0;
  let modelHsdDobjs = 0;
  let modelHsdMobjs = 0;
  let modelHsdPobjs = 0;
  let modelHsdTobjs = 0;
  let modelHsdRobjs = 0;
  for (let table = 0; table < articleCounts.length; ++table) {
    const base = p[table + 1];
    if (base === null) die(`${file}: missing Article table ${table}`);
    dat.requireSpan(base, articleCounts[table] * 4, `Article table ${table}`);
    for (let i = 0; i < articleCounts[table]; ++i) {
      const article = dat.pointer(base + i * 4);
      if (article !== null) articles.add(article);
    }
  }

  for (const article of articles) {
    dat.requireSpan(article, 0x18, 'Article');
    const fields = Array.from({ length: 6 }, (_, i) => dat.pointer(article + i * 4));
    if (fields[0] !== null) attrs.add(fields[0]);
    if (fields[2] !== null) hurts.add(fields[2]);
    if (fields[4] !== null) models.add(fields[4]);
    if (fields[5] !== null) dynamics.add(fields[5]);
  }

  for (const attr of attrs) {
    dat.requireSpan(attr, 0x84, 'ItemAttr');
    assertNoRelocations(dat, attr, 0x84, 'ItemAttr');
  }
  for (const hurt of hurts) {
    dat.requireSpan(hurt, 8, 'ItHurtBoneList');
    const count = dat.word(hurt);
    const descs = dat.pointer(hurt + 4);
    if (count > 2 || (count && descs === null)) {
      die(`${file}: invalid ItHurtBoneList count ${count}`);
    }
    if (count) {
      dat.requireSpan(descs, count * 0x20, 'ItHurtBoneDesc');
      assertNoRelocations(dat, descs, count * 0x20, 'ItHurtBoneDesc');
    }
  }
  for (const model of models) {
    dat.requireSpan(model, 0x10, 'ItemModelDesc');
    const joint = dat.pointer(model);
    const boneCount = dat.word(model + 4);
    const attachId = dat.word(model + 8) | 0;
    if (boneCount > 100 || attachId < -1 || attachId > 100) {
      die(`${file}: invalid ItemModelDesc ${boneCount}/${attachId}`);
    }
    assertNoRelocations(dat, model + 4, 8, 'ItemModelDesc scalars');
    if (joint !== null) {
      const hsd = auditFighterPartsHsd(dat, joint);
      modelHsdRoots++;
      modelHsdJoints += hsd.joints;
      modelHsdDobjs += hsd.dobjs;
      modelHsdMobjs += hsd.mobjs;
      modelHsdPobjs += hsd.pobjs;
      modelHsdTobjs += hsd.tobjs;
      modelHsdRobjs += hsd.robjs;
    }
  }
  for (const dyn of dynamics) {
    dat.requireSpan(dyn, 0x10, 'ItemDynamics');
    const boneCount = dat.word(dyn);
    const boneDescs = dat.pointer(dyn + 4);
    const collCount = dat.word(dyn + 8);
    const collDescs = dat.pointer(dyn + 0x0c);
    if (boneCount > 24 || collCount > 2 ||
        (boneCount && boneDescs === null) || (collCount && collDescs === null)) {
      die(`${file}: invalid ItemDynamics counts ${boneCount}/${collCount}`);
    }
    for (let i = 0; i < boneCount; ++i) {
      const desc = boneDescs + i * 0x18;
      dat.requireSpan(desc, 0x18, 'BoneDynamicsDesc');
      const boneId = dat.word(desc) | 0;
      const source = dat.pointer(desc + 4);
      const count = dat.word(desc + 8);
      if (boneId < 0 || boneId >= 100 || count > 32 || (count && source === null)) {
        die(`${file}: invalid BoneDynamicsDesc ${boneId}/${count}`);
      }
      assertNoRelocations(dat, desc, 0x18, 'BoneDynamicsDesc', new Set([4]));
      if (count) {
        dynamicsSources.add(`${source}:${count}`);
        dat.requireSpan(source, count * 0x3c, 'item dynamics source');
        assertNoRelocations(dat, source, count * 0x3c, 'item dynamics source');
      }
    }
    if (collCount) {
      dat.requireSpan(collDescs, collCount * 0x14, 'ItCollDynamicsDesc');
      assertNoRelocations(dat, collDescs, collCount * 0x14, 'ItCollDynamicsDesc');
    }
  }
  if (p[4] !== null) dat.requireSpan(p[4], 0x1c, 'item scalar globals');
  return {
    articles: articles.size,
    attrs: attrs.size,
    hurts: hurts.size,
    models: models.size,
    modelHsdRoots,
    modelHsdJoints,
    modelHsdDobjs,
    modelHsdMobjs,
    modelHsdPobjs,
    modelHsdTobjs,
    modelHsdRobjs,
    dynamics: dynamics.size,
    dynamicsSources: dynamicsSources.size,
  };
}

function main() {
  if (!fs.existsSync(assets)) die(`asset directory not found: ${assets}`);
  const fighterFiles = fs.readdirSync(assets)
    .filter((name) => /^Pl..\.dat$/.test(name) && name !== 'PlCo.dat')
    .sort();
  if (fighterFiles.length !== 33) die(`expected 33 fighter DATs, got ${fighterFiles.length}`);
  let motions = 0;
  let demos = 0;
  let costumeTobjArrays = 0;
  let costumeTobjIndices = 0;
  let visLookups = 0;
  let visTemps = 0;
  let visTempEntries = 0;
  let visByteIndices = 0;
  let partsHsdFiles = 0;
  let partsHsdNull = 0;
  let partsHsdJoints = 0;
  let partsHsdDobjs = 0;
  let partsHsdMobjs = 0;
  let partsHsdPobjs = 0;
  let partsHsdTobjs = 0;
  for (const name of fighterFiles) {
    const result = auditFighter(path.join(assets, name));
    motions += result.motionCount;
    demos += result.demoCount;
    costumeTobjArrays += result.costumeTobjArrays;
    costumeTobjIndices += result.costumeTobjIndices;
    visLookups += result.visLookups;
    visTemps += result.visTemps;
    visTempEntries += result.visTempEntries;
    visByteIndices += result.visByteIndices;
    if (result.partsHsd.present) partsHsdFiles++;
    else partsHsdNull++;
    partsHsdJoints += result.partsHsd.joints;
    partsHsdDobjs += result.partsHsd.dobjs;
    partsHsdMobjs += result.partsHsd.mobjs;
    partsHsdPobjs += result.partsHsd.pobjs;
    partsHsdTobjs += result.partsHsd.tobjs;
  }
  const plco = auditPlCo(path.join(assets, 'PlCo.dat'));
  const itcoDat = auditItCo(path.join(assets, 'ItCo.dat'));
  const itcoUsd = auditItCo(path.join(assets, 'ItCo.usd'));
  const colorHist = [...plco.colorCommands.histogram.entries()].sort((a, b) => a[0] - b[0]).map(([op, n]) => `${op}:${n}`).join(',');
  console.log(`GAMEPLAY_DAT_AUDIT_PASS fighters=${fighterFiles.length} motions=${motions} demos=${demos} costume_tobj_arrays=${costumeTobjArrays} costume_tobj_indices=${costumeTobjIndices} vis_lookups=${visLookups} vis_temp_arrays=${visTemps} vis_temp_entries=${visTempEntries} vis_byte_indices=${visByteIndices} fighter_parts_hsd=${partsHsdFiles}/${partsHsdNull} fighter_parts_nodes=${partsHsdJoints}/${partsHsdDobjs}/${partsHsdMobjs}/${partsHsdPobjs}/${partsHsdTobjs} plco=1 plco_special_parts=${plco.specialPartsRecords}/${plco.specialPartsEntries} plco_model_shift_vecs=${plco.modelShiftVectors} plco_common_hsd=${plco.commonModels.map((x) => x.joints).join('/')} plco_common_anim=${plco.commonAnimJoints} plco_color_cmds=${plco.colorCommands.commands}[${colorHist}] plco_cpu_attacks=${plco.cpuAttackLists}/${plco.cpuAttackEntries} itco=2 articles=${itcoDat.articles}/${itcoUsd.articles} item_model_hsd=${itcoDat.modelHsdRoots}/${itcoUsd.modelHsdRoots} item_model_nodes=${itcoDat.modelHsdJoints}/${itcoDat.modelHsdDobjs}/${itcoDat.modelHsdMobjs}/${itcoDat.modelHsdPobjs}/${itcoDat.modelHsdTobjs}/${itcoDat.modelHsdRobjs}:${itcoUsd.modelHsdJoints}/${itcoUsd.modelHsdDobjs}/${itcoUsd.modelHsdMobjs}/${itcoUsd.modelHsdPobjs}/${itcoUsd.modelHsdTobjs}/${itcoUsd.modelHsdRobjs} hurts=${itcoDat.hurts}/${itcoUsd.hurts} dynamics=${itcoDat.dynamics}/${itcoUsd.dynamics}`);
}

try {
  main();
} catch (error) {
  console.error(`GAMEPLAY_DAT_AUDIT_FAIL ${error.message}`);
  process.exit(1);
}
