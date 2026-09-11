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
    for (const wordOff of [0, 0x20]) {
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
  return { name, motionCount, demoCount: demoBytes / 0x18 };
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
    dat.pointer(model);
    const boneCount = dat.word(model + 4);
    const attachId = dat.word(model + 8) | 0;
    if (boneCount > 100 || attachId < -1 || attachId > 100) {
      die(`${file}: invalid ItemModelDesc ${boneCount}/${attachId}`);
    }
    assertNoRelocations(dat, model + 4, 8, 'ItemModelDesc scalars');
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
  for (const name of fighterFiles) {
    const result = auditFighter(path.join(assets, name));
    motions += result.motionCount;
    demos += result.demoCount;
  }
  auditPlCo(path.join(assets, 'PlCo.dat'));
  const itcoDat = auditItCo(path.join(assets, 'ItCo.dat'));
  const itcoUsd = auditItCo(path.join(assets, 'ItCo.usd'));
  console.log(`GAMEPLAY_DAT_AUDIT_PASS fighters=${fighterFiles.length} motions=${motions} demos=${demos} plco=1 itco=2 articles=${itcoDat.articles}/${itcoUsd.articles} hurts=${itcoDat.hurts}/${itcoUsd.hurts} dynamics=${itcoDat.dynamics}/${itcoUsd.dynamics}`);
}

try {
  main();
} catch (error) {
  console.error(`GAMEPLAY_DAT_AUDIT_FAIL ${error.message}`);
  process.exit(1);
}
