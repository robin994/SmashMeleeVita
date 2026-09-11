# Porting status - 2026-09-11, v3.44 Classic VS/loading + StageCallbacks PPC-bit fix

## 2026-09-11 — v3.44: restore Classic VS/loading scene and fix PPC StageCallbacks flags on ARM

The physical-v3.43 result proved the nested ItCo conversion and moved the failure to Ground_801C43C4. The runtime still had the temporary MELEE_VITA_SKIP_CLASSIC_INTRO=ON checkpoint, so Classic skipped GS_INTRO_EASY, the retail VS/loading presentation that should appear after character select, and entered GS_VS directly. v3.44 disables that bypass and restores the original scene transition.

The later stage-light fallback was also a PPC/ARM ABI issue rather than corrupt stage data. StageCallbacks flags are initialized by the retail stage tables with PPC MSB-first masks (1<<31, 1<<30, 1<<29), while ARM bitfield allocation reads the opposite side of the storage byte. Ground now tests the original u32 masks explicitly on Vita for light, fog and camera callbacks and logs VITA_STAGE_CALLBACK_FLAGS.

GmIntEz.dat/gmIntroEasyTable is scalar-only and has no relocations. v3.44 adds a bounded pre-relocation converter for the 448 typed float fields used by gm_1832.c while preserving byte/padding regions. Successful conversion emits VITA_CLASSIC_EASY_TABLE_NATIVE_PASS file=GmIntEz.dat floats=448 bytes=2488.

Runtime marker is MELEE_VITA_GAME_BOOT v3.44; VPK APP_VER is 00.54; MELEE_VITA_SKIP_CLASSIC_INTRO=OFF. The authoritative source checkout builds through ARM compile/link, VELF, SELF and VPK with exit 0, and git diff --check passes. Hardware artifact: build/vita-full/SmashMeleeVita-v3.44-classic-loading-stageflags.vpk, 3,194,565 bytes, SHA-256 f4b511b0709f6232646e09103220be4bf857ed44de5c9bba15f0890cc6f6116b. eboot.bin is 2,763,763 bytes, SHA-256 8d78a1e480c332530b65695d44e7eb6dbe06c8df4d5a2fbf6878d1d69440ac35. The unstripped ELF is 9,196,944 bytes, SHA-256 e167e72d2fda3fbc5ca3eab7a709f3c096cabe0a09b91ecf11692cdf9da9f272. Physical-Vita validation remains pending.

Next physical test: Classic -> select character -> START. The expected first visible milestone is the retail VS/loading presentation. VITA_CLASSIC_INTRO_BYPASS must be absent; if the match reaches stage setup, VITA_STAGE_CALLBACK_FLAGS should show the retail MSB masks decoded into the correct booleans.

## 2026-09-11 — v3.43: fix the physical-v3.42 ItCo item-collision panic at its serialized-data boundary

The physical-v3.42 run advances through the systemic gameplay-DAT pass, stage setup and collision setup, then stops deliberately in `it_8027163C()` with `item hit num over!` / `itcoll.c:1021`. The supplied core confirms an assert/panic path (`stop_reason=0x30002`, IFAR/DFAR zero), not an ARM Data Abort. Symbolization lands in `HSD_Panic -> it_8027163C`, matching the runtime log.

The crashing object is identifiable from the core rather than inferred: `HSD_GObj=0x8235ad40`, `Item=0x8235ada0`, `Item.kind=0xA0`. The runtime item routing treats this as the last entry of the character-item table (index 117). The corresponding retail `ItCo.dat` Article has `ItHurtBoneList.count=1`, serialized as big-endian `00 00 00 01`. v3.42 relocated the surrounding pointers but did not yet nativeize nested `Article` payloads, so ARM read that scalar as `0x01000000` and deterministically tripped the retail `count > 2` assertion.

v3.43 extends `vita/gameplay_archive_vita.c` at the existing pre-relocation ItCo boundary instead of weakening the collision assertion. It walks the fixed retail Article pointer tables (43 common, 118 character, 47 Pokemon entries), deduplicates shared targets, and nativeizes the scalar portions of `ItemAttr`, `ItHurtBoneList`/`ItHurtBoneDesc`, `ItemModelDesc`, `ItemDynamics`/`BoneDynamicsDesc`, the serialized `0x3C` dynamics-source records, and `ItCollDynamicsDesc`. Relocation-managed pointers remain byte-identical. `ItemAttr` receives an explicit PPC-MSB-first to ARM-LSB-first bitfield remap for its first two bytes; the model descriptor's explicit byte mask remains untouched. Bounds are fail-closed (`hurtbox <= 2`, collision dynamics <= 2, bone dynamics <= 24, source count <= 32) and malformed layouts emit `VITA_ITCO_HURTBOX_INVALID` / `VITA_ITCO_DYNAMICS_INVALID` before panic.

The permanent PAL asset audit now validates the same nested schema in both `ItCo.dat` and `ItCo.usd`: `GAMEPLAY_DAT_AUDIT_PASS fighters=33 motions=10658 demos=471 plco=1 itco=2 articles=94/94 hurts=24/24 dynamics=3/3`. Across each ItCo archive there are 94 unique reachable Articles, 91 unique ItemAttr blocks, 24 unique hurtbox lists, 92 unique model descriptors and three dynamics blocks; every retail hurtbox count is 1 or 2. Runtime collision asserts are retained, with Vita-only diagnostics `VITA_ITEM_HURTBOX_COUNT_INVALID` and `VITA_ITEM_DYNAMICS_COUNT_INVALID` added immediately before them so any later format mismatch identifies the exact item/article/count.

Runtime marker is `MELEE_VITA_GAME_BOOT v3.43`; VPK APP_VER is `00.53`. Both the managed worktree and authoritative source checkout build through ARM compile/link, VELF, SELF and VPK successfully. `git diff --check` and the expanded PAL gameplay-DAT audit pass in the source checkout. Hardware artifact: `build/vita-full/SmashMeleeVita-v3.43-itco-article-hurt-dynamics.vpk`, 3,194,674 bytes, SHA-256 `b323624c210d0c47dd388a56ff5ce81de3ec1d11ab6f3d9fae657117a4b13207`; it is byte-for-byte identical to the freshly generated `SmashMeleeVita-assets.vpk`. `eboot.bin` is 2,763,632 bytes with SHA-256 `7cd529015ef0d2c6ddcde8534fbea353015578b46a85eab8f0128420f5355c00`. The unstripped `melee_vita` ELF is 9,196,648 bytes with SHA-256 `1fe803bbc148053bca971e6804e0c1a09dedcc2b9af43a7bd55a695b4fbee05f`; it contains the v3.43/upstream markers plus all new ItCo/collision diagnostics. The packaged SFO is 912 bytes, has valid `\0PSF` magic and reports APP_VER `00.53`. Physical-Vita validation of v3.43 is still pending.

Next physical test: install v3.43 and repeat Classic -> character -> START. Expected early proof is `VITA_ITCO_NATIVE_PASS ... articles=94 attrs=91 hurts=24 models=92 dynamics=3 ...`; the old `item hit num over!` / `itcoll.c:1021` stop should disappear. Any later crash should be treated as the next genuine subsystem reached, not bypassed by removing the retained collision invariants.

## 2026-09-11 — v3.42: nativeize the complete fighter/common gameplay DAT boundary before returning to hardware

v3.42 is deliberately a systemic pass rather than another downstream crash patch. It is layered on top of the real v3.41 stage-spline fix below, so the `GrKg.dat` raw-spline validation correction and its stage regression tests are preserved. The new boundary addresses the next broad class of failures: ordinary gameplay containers whose pointer words are relocated correctly by HSD but whose numeric payload remains GameCube big-endian when consumed by ARM.

`vita/gameplay_archive_vita.c` now runs before HSD relocation for three high-impact PAL archive families: all 33 `PlXX.dat` fighter roots, `PlCo.dat`, and `ItCo.dat`/`ItCo.usd`. DAT relocation metadata is authoritative: relocation fields are never byteswapped, typed scalar regions are converted, byte-packed/padding/color regions are explicitly preserved, and an unexpected layout fails closed with `VITA_GAMEPLAY_DAT_INVALID` instead of being allowed to become a later data abort or assertion.

The fighter audit covers every PAL `ftData*` root. All 33 archives expose the same 24-word pointer/null root layout; this also fixes a decomp ABI declaration where `ftData.x54` was typed as `int` even though every retail DAT marks it as a relocation and runtime consumers use it as an `int*`. The primary `Fighter_WaitAnimData` tables contain 10,658 records across the roster and the demo tables contain another 471. Every primary record has `x8 <= 0x8000` and every `x4 + x8` range remains inside its matching `PlXXAJ.dat`. The converter therefore nativeizes the scalar fields of both motion tables while preserving their relocation-managed references.

The pass also covers the structures consumed immediately after fighter creation: `ftCo_DatAttrs`, character-specific `ext_attr`, `ftData_x8` counts, wait-animation tables, model-part records, `ftDynamics`/`BoneDynamicsDesc`, hurtbox initializers, collision-dynamics records, camera vectors, item-pickup vectors, ledge metadata, SFX tables, `x50`, the corrected pointer table at `x54`, and the mixed `x58` record. Character-specific attributes are schema-aware rather than blindly word-swapped: byte/filler ranges in Popo/Nana/Purin/Yoshi are preserved, Marth/Roy's byte-packed `SwordAttrs` region is preserved, `ReflectDesc` behavior bytes are preserved, and Kirby's isolated `s16` field is swapped as a halfword. Serialized fighter dynamics were traced through `lb_80011710`; their source payload is a contiguous `lb_00F9_UnkDesc1Inner[count]` table (`0x3C` bytes per record), not runtime `DynamicsData`, and v3.42 converts that actual serialized representation.

`PlCo.dat` is normalized from its 23-pointer `ftLoadCommonData` root. The `0x818`-byte `ftCommonData`, swing/stale tables, fighter-parts counts, shake tables, scale-modifier table and `CrowdConfig` are converted with `GXColor` and byte regions left byte-identical. `ItCo.dat` and `ItCo.usd` are recognized through `itPublicData`; their six root references remain relocation-managed while the `0x160`-byte `ItemCommonData` and known scalar globals are converted with byte-packed holes preserved.

A permanent asset regression, `vita/tools/audit_gameplay_dat.js`, validates the schema against the original PAL data before packaging. Current result: `GAMEPLAY_DAT_AUDIT_PASS fighters=33 motions=10658 demos=471 plco=1 itco=2`. A relocation-overlap audit additionally confirms that no scalar range converted by v3.42 contains a hidden relocation pointer. Audit of the internal `ftData.x1C` HSD tables found two Kirby-specific mixed records, so v3.42 intentionally does not add a blind generic HSD traversal there; existing typed HSD nativeizers remain responsible for recognized HSD graphs.

Runtime marker is `MELEE_VITA_GAME_BOOT v3.42`; VPK APP_VER is `00.52`. New success telemetry is `VITA_FIGHTER_DATA_NATIVE_PASS`, `VITA_PLCO_NATIVE_PASS`, and `VITA_ITCO_NATIVE_PASS`. The final source-checkout build completes through ARM link, VELF, SELF and VPK with exit 0. `git diff --check` and the full PAL gameplay-DAT regression pass after packaging. Hardware artifact: `build/vita-full/SmashMeleeVita-v3.42-gameplay-dat-nativeizer.vpk`, 3,191,731 bytes, SHA-256 `2be699f58457fa8b4a0015aad6ccb7e1e998eab1d603cd1145434e17f06a5546`; it is byte-for-byte identical to the generated `SmashMeleeVita-assets.vpk`. `eboot.bin` is 2,760,932 bytes with SHA-256 `6012e1102d10580cd4538831b899d068dfa3ea32dbbf3ff4826169ec347e374c`. The unstripped `melee_vita` ELF is 9,189,948 bytes with SHA-256 `2ff3b3fda036885df22f226f8b87a8f9642d7bc17a71f18f13852898e5e8b86e`; it contains the v3.42/upstream markers together with `VITA_STAGE_HSD_RAW_NATIVE_PASS`, proving the v3.41 stage-spline path remains linked. The packaged SFO is 912 bytes, has valid `\0PSF` magic and reports APP_VER `00.52`. No physical-Vita validation of v3.42 is claimed yet.

## 2026-09-11 — v3.41: fix the GrKg stage spline rejection after CSS

Latest physical input is `build/vita-full/runtime.log` (v3.40), plus
`psp2core-1789127215-0x001a7327b5-eboot.bin.psp2dmp`. The log reaches Classic CSS
exit (character kind 8), loads EfCoData and EfMnData successfully, and then
panics at `stage_archive_vita.c:1256`: map 1, root `0x3afe8`, JObj `0x3b410`,
flags `0x4008`. These exact offsets identify `GrKg.dat`. This is a valid
`JOBJ_SPLINE | JOBJ_CLASSICAL_SCALE`, not a corrupt descriptor. The supplied
gzip coredump was decoded as an ARM ELF core (`latest-core.elf`); its main-thread
PC is `0x8136b3ea`, LR `0x8136b3b7`, and kill arguments carry signal 6, consistent
with the logged HSD_Panic -> abort path. No GPU-fault conclusion is drawn.

Root cause: the stage preflight still called the strict compact-menu builder
`mv_hsd_native_build_at`, although the effect path already used
`mv_hsd_native_validate_raw_at`. The raw stage walker and original JObj loader
already support typed splines. Stage preflight now uses the raw validator too;
compact menu-proxy restrictions are unchanged. Stage success telemetry now
includes `splines=`. No assertion was removed and no scene was bypassed.

Validation:
- `vita/tools/test_stage_spline.py` on the linked ARM ELF: PASS. Reproduces the
  strict proxy rejection, accepts the same root through raw validation, rejects
  a corrupted spline count, nativeizes and relocates the real archive, then
  executes original `HSD_JObjLoadJoint` and `HSD_JObjRemoveAll` successfully.
- `vita/tools/test_stage_raw.py`: 68/76 retail stage archives pass nativeization,
  relocation-field preservation and metadata preservation. Seven large stages
  required a larger emulator instruction budget and pass when the same call is
  resumed. Eight genuine unsupported layouts remain; this is not a whole-game
  success claim. Full details: `build/vita-full/v3.41-verification.json`.
- Remaining audit failures: GrNSr.dat: stage raw HSD graph unsupported; GrPs.dat: stage raw JObj pointer invalid; GrPs.usd: stage raw JObj pointer invalid; GrPs1.dat: stage raw JObj pointer invalid; GrPs2.dat: stage raw JObj pointer invalid; GrPs3.dat: stage raw JObj pointer invalid; GrPs4.dat: stage raw JObj pointer invalid; GrSt.dat: stage raw HSD graph unsupported.
  Pokémon Stadium variants contain non-relocated `0xffffffff` map-root slots;
  their runtime semantics must be integrated before accepting them. Do not
  classify these assets as missing/corrupt or silently turn the slots into NULL.
- Build/link/VELF/SELF/VPK exit 0. ZIP CRC, APP_VER 00.51 and packaged eboot match
  verified. Existing linker enum-size warnings remain.
- VPK: `build/vita-full/SmashMeleeVita-v3.41-stage-spline.vpk`, 3185566 bytes.
  SHA-256: `f2555ba94278e74c4ac29db29b88cb58b9b535d5660c8601b472b8302c2f6276`.
- Logs: `v3.41-build.log`, `stage-spline-regression.log`, `stage-raw-check.log`,
  `stage-remaining-rejections.log` in `build/vita-full`.

Next physical test: install v3.41, repeat Classic -> same character -> START.
Expected: the old GrKg map-1 panic disappears and
`VITA_STAGE_HSD_RAW_NATIVE_PASS ... splines=...` appears. Collect the resulting
log/core for whatever follows; no v3.41 device validation has occurred here.
Continue full native/vitaGL integration; do not reintroduce artificial frontier
pauses. Remaining stage formats and subsequent gameplay callbacks are still
work to do; this update does not claim a playable match.


## 2026-09-11 — v3.40: accept particle-only effect archives and raw PBILLBOARD models

The v3.39 physical-Vita run proves the complete common-effect nativeizer is now working on hardware. `EfCoData.dat` reaches `VITA_EFFECT_HSD_RAW_NATIVE_PASS ... effects=47 ... splines=1 shapejoints=37 shapedobjs=22`, then its particle bank is converted successfully with `VITA_PS_BANK_NATIVE_PASS`. The previous effect-3 type-40 AnimJoint rejection, PATH/spline handling and ShapeAnim alignment-tree blockers are therefore cleared on a real Vita. Execution advances to the second synchronous effect bank, `EfMnData.dat`, where the raw validator deliberately stops with `VITA_EFFECT_HSD_RAW_INVALID ... effects=0` and `effect descriptor table invalid`.

`EfMnData.dat` is not malformed. The PAL archive exports `effMenuDataTable` at data offset zero and contains exactly two relocation fields: `+0 -> 0x20` and `+4 -> 0x360`. Those are the particle command and texture banks consumed by `efAsync_LoadSync()`/`psInitDataBank*`; there is intentionally no inline `EF_EffectDesc[]` beginning at `+8`. Auditing every PAL `Ef*.dat` shows the same valid particle-only layout in five archives: `EfMnData.dat`, `EfKbIc.dat`, `EfKbKp.dat`, `EfKbPc.dat` and `EfKbPk.dat`. v3.40 therefore accepts `effects=0` only when both root particle-bank fields are genuine DAT relocations, emits `VITA_EFFECT_HSD_RAW_PARTICLE_ONLY_PASS`, leaves those pointer offsets untouched for `HSD_ArchiveParse()`, and returns before any model walk. A zero-effect root without both bank relocations still fails closed.

The full retail audit also found and removed the next conservative-validator blocker before another hardware round-trip. `EfNsData.dat` effect 2 contains JObj flags `0x00082208`; the additional `0x2000` bit is the normal HSD `JOBJ_PBILLBOARD` mode. Upstream `displayfunc.c` handles PBILLBOARD only as an alternate billboard matrix calculation; it does not change the serialized JObj union schema. v3.40 consequently permits PBILLBOARD in pre-relocation raw validation, alongside the V/H/R billboard modes already enabled in v3.39, while the compact standalone native-proxy validator remains deliberately stricter and still rejects it.

The linked ARM Vita ELF was then run against every original PAL effect archive containing an `eff*DataTable` public symbol. All 36 archives pass the real `mv_effect_archive_prepare_raw()` path, including all five particle-only roots, `EfNsData.dat` with PBILLBOARD, `EfCoData.dat` with its type-40/PATH/spline data and the character/Kirby effect banks. This moves validation from one crash-at-a-time asset sampling to the complete retail effect-DAT family currently present on the disc.

Runtime marker is `MELEE_VITA_GAME_BOOT v3.40`; VPK APP_VER is `00.50`. Physical-Vita target remains Classic -> select character -> START. The decisive new marker after the already-proven `EfCoData.dat` pass is `VITA_EFFECT_HSD_RAW_PARTICLE_ONLY_PASS file=EfMnData.dat root=effMenuDataTable ... effects=0`, followed by the normal particle-bank initialization. The old `effect descriptor table invalid` panic at `stage_archive_vita.c:1021` must disappear; any later stop should now be beyond every effect archive schema exercised by the PAL asset set.

The final source-checkout build completes through VELF/SELF/VPK. Hardware artifact: `build/vita-full/SmashMeleeVita-v3.40-effect-particle-only-pbillboard.vpk`, 3,185,469 bytes, SHA-256 `3a1ec8e1878ce95f10a17bc0d5f3b1e7e3885d0ec3e157c70853023d286b2514`; it is byte-for-byte identical to `SmashMeleeVita-assets.vpk`. `eboot.bin` is 2,754,638 bytes with SHA-256 `46f3f92d79edd14cf1e1895fb65a9afd45f775bd6652adea23b12825e468e327`. The unstripped ELF is 9,174,736 bytes with SHA-256 `396fe5e204887b0b9edd90479571d56e485041790ad24c61cf6a793ff1ee6176` and contains the v3.40/upstream markers plus both effect-nativeization success markers. The generated SFO has valid `\0PSF` magic and reports APP_VER `00.50`.

## 2026-09-11 — v3.39: accept retail effect callback channels and nativeize PATH splines

The v3.38 physical-Vita run advances past the previous stage/light and shared-skin failure points, then stops deliberately during the first common-effect archive. `EfCoData.dat` is recognized by the new raw hook, but validation of effect 3 fails before relocation with `VITA_EFFECT_HSD_RAW_VALIDATE_FAIL ... effect=3 kind=anim code=-1 off=0011238c`. The coredump is the expected panic path from `stage_archive_vita.c`, not a random ARM data abort, so the failure is another conservative schema rejection rather than memory corruption.

Inspection of the exact PAL `EfCoData.dat` shows the rejected descriptor is a valid `HSD_FObjDesc` at data offset `0x1121c0`: `length=11`, `startframe=0`, and animation `type=0x28` (40). Upstream `JObjUpdateFunc()` explicitly implements `case 0x28` as the dynamic-particle callback, and `efLib_Init()` registers `efLib_Cb_DPtcl` with `HSD_JObjSetDPtclCallback()`. The v3.38 converter incorrectly treated every JObj FObj type above 12 as malformed. A complete audit of all 47 `effCommonDataTable` entries finds serialized types `1..10`, `12`, and `40`; type 40 appears throughout the common-effect bank and is therefore part of the normal retail format.

The same audit found the next valid feature before requiring another hardware round-trip. Effect 36 contains the only serialized `HSD_A_J_PATH` channel (type 4) and its `HSD_AObjDesc.obj_id` field is a DAT relocation, not a scalar: it targets JObj `0x12a8e8`, whose flags are `JOBJ_SPLINE | JOBJ_CLASSICAL_SCALE`. Its union points to a 24-byte `HSD_Spline` at `0x12a8d0`: type 2, `numcv=9`, control points at `0x12a788`, segment lengths at `0x12a80c`, and segment polynomials at `0x12a830`. The serialized array extents line up exactly with the next objects: 11 Vec3 control points, 9 segment-length floats, and 8×5 polynomial coefficients.

v3.39 therefore changes the raw-effect boundary rather than adding another one-off swap. JObj animation validation accepts the upstream scalar callback range `20..42` in addition to channels `1..12`; the compact title/menu sample surfaces still only store the ordinary twelve channels. `HSD_AObjDesc.obj_id` is now validated through relocation metadata and left byte-identical until `HSD_ArchiveParse()` performs normal pointer relocation. The raw walker converts spline scalar fields plus all control-point/length/polynomial floats while leaving its three pointer fields untouched. `JOBJ_SPLINE` is permitted by the Vita load-only constructor once its descriptor has passed that typed conversion; `INSTANCE` and `PTCL` remain fail-closed.

The effect model validator now has a separate raw-validation mode instead of weakening the standalone title/menu native proxy. This mode accepts the standard HSD V/H/R billboard modes and typed spline JObjs used by `EfCoData`, while continuing to reject instance/particle unions, quaternion/user matrices, unsupported joint classes and RObj references. Auditing all 47 common-effect model roots with this raw mode produces zero failures. Ten effects also contain `HSD_ShapeAnimJoint` alignment trees; every serialized `shapeanim` payload pointer in this archive is null, so v3.39 preserves/traverses those pointer-only Joint/DObj trees but deliberately remains fail-closed if a real ShapeAnim payload appears in another asset.

The complete regression chain was executed with the linked ARM Vita ELF against the original PAL `EfCoData.dat`. Both the exact v3.38 effect-3 AnimJoint and effect 36 PATH AnimJoint validate; all 47 effect models validate; the mutating raw nativeizer passes with type-40, `obj_id` relocation and spline pointer bytes checked; the real `HSD_ArchiveParse()` then relocates the archive; and the real `HSD_JObjLoadJoint()` + `HSD_JObjAddAnimAll()` + frame-0 animation path succeeds for effects 3 and 36. This tests both the current hardware failure and the next spline-dependent feature before returning to Vita.

Runtime marker is `MELEE_VITA_GAME_BOOT v3.39`; VPK APP_VER is `00.49`. The expected common-effect marker now includes `splines=1` plus the structural ShapeAnim counts, e.g. `VITA_EFFECT_HSD_RAW_NATIVE_PASS file=EfCoData.dat ... splines=1 ...`. The old `effect=3 kind=anim code=-1` panic must disappear. The next physical-Vita run should repeat Classic -> select character -> START and report the next genuine subsystem reached after the common-effect archive.

## 2026-09-11 — v3.38: nativeize fighter/effect HSD archives and resolve shared-skin PObjs

The v3.37 physical-Vita run proves the previous stage-loader and shadow/light fixes are effective. The GS_VS stage now emits `VITA_STAGE_HSD_RAW_NATIVE_PASS file=grDatFiles_async ...`, completes joint-map, collision, item and stage normalization, reuses its particle bank, and no longer hits the old `ground.c:2526` light/shadow assertion. Execution advances into fighter setup and then stops deliberately at `pobj.c:298` with `load-only shared-skin PObj requires native adapter`.

The v3.37 core shows another instance of the same systemic DAT ABI problem, but in a different preload class. The active relocated `HSD_PObjDesc` is at `0x82a174d8`: its pointer fields are already valid Vita addresses (`verts=0x82a096f8`, `display=0x82a17480`, `joint=0x82a174c0`), while bytes `20 01 00 02` at the two serialized `u16` fields are observed by ARM as `flags=0x0120` and `n_display=0x0200`. Their correct GameCube values are `flags=0x2001` and `n_display=2`.

Matching the descriptor against the original disc assets identifies the source exactly as `PlMrNr.dat`, not `EfMrData.dat`. `PlMrNr.dat` exports `PlyMario5K_Share_joint` at data offset `0x19400` and `PlyMario5K_Share_matanim_joint` at `0x44590`; relocation metadata identifies the crashing `HSD_PObjDesc` at data offset `0x193d8`, with the exact `0x2001 / 2` scalar pair and relocated-reference fields at `+0x08`, `+0x10` and `+0x14`. Subtracting `0x193d8` from the core address gives runtime archive-data base `0x829fe100`, which places the public model root at `0x82a17500`, confirming the match.

The loader-path cause is also now explicit. Fighter costume archives are preloaded by `ftData` through `lbDvd_800178E8(... type=2 ...)`, and `lbDvd_GetPreloadedArchive()` type 2 previously called `lbArchive_InitializeDAT()` directly. That bypassed the generic `PREPARE_RAW()` hook, just as type 4 stage preloads did before v3.37. v3.38 runs the schema-aware raw archive dispatcher before type-2 relocation, so `Ply*5K_Share_joint` fighter/costume archives are nativeized while their references are still serialized offsets. Type-3 effect preloads are hooked as well, preventing the same mixed-endian HSD state in `eff*DataTable` archives.

The raw schema has been extended rather than adding a one-off Mario swap. Fighter archives are recognized by `Ply*5K_Share_joint` plus optional `Ply*5K_Share_matanim_joint`; their JObj/DObj/MObj/PObj/TObj/vertex/image/TLUT graphs and MatAnim/AObj/FObj/TexAnim scalar descendants are converted in place while pointer fields and byte streams remain untouched. Effect archives are recognized by `eff*DataTable`; the inline `EF_EffectDesc` table is bounded using relocation metadata and its lifetime, model, AnimJoint and MatAnim data are validated/nativeized before relocation. Unsupported shape/render animation still fails closed.

Shared-skin itself now has a real load-only adapter. `PObjLoad()` no longer rejects a non-null POBJ_SKIN JObj descriptor reference; `HSD_PObjResolveRefs()` resolves it after the full JObj tree has been registered using the upstream HSD ID table, retains the reference, and emits `VITA_POBJ_SHARED_SKIN_RESOLVE_PASS`. The Vita capture path mirrors retail `SetupSharedVtxModelMtx`: PNMTX0 receives the owner matrix and PNMTX1 the resolved shared-vertex JObj matrix instead of incorrectly treating shared skinning as an ordinary rigid PObj.

The exact crash path was regression-tested against the original PAL assets with the linked ARM Vita ELF. For `PlMrNr.dat`, the raw nativeizer converts the crashing four bytes `20 01 00 02 -> 01 20 02 00` while relocation fields remain byte-identical; the real `HSD_ArchiveParse()` then succeeds and the real `HSD_JObjLoadJoint()` completes with the shared-skin reference resolved through the HSD ID table. `EfMrData.dat` is also tested through the new effect nativeizer, including its big-endian `13.0f` lifetime (`41 50 00 00 -> 00 00 50 41`). This reproduces and passes the constructor chain that v3.37 stopped inside.

Runtime marker is `MELEE_VITA_GAME_BOOT v3.38`; VPK APP_VER is `00.48`. The full gameplay profile remains `MELEE_VITA_FULL_GAMEPLAY_SCENE=ON` with `MELEE_VITA_SKIP_CLASSIC_INTRO=ON` while the first real match remains the active hardware target.

Physical-Vita target: repeat Classic -> select character -> START. The key new markers are `VITA_FIGHTER_HSD_RAW_NATIVE_PASS ... root=PlyMario5K_Share_joint ...`, `VITA_POBJ_SHARED_SKIN_RESOLVE_PASS ...`, and, when the Mario effect archive is parsed, `VITA_EFFECT_HSD_RAW_NATIVE_PASS ... root=effMarioDataTable effects=2 ...`. The old `pobj.c:298: load-only shared-skin PObj requires native adapter` panic must disappear. Any later failure should now identify the next archive schema or runtime subsystem rather than re-entering this fixed mixed-endian/shared-skin path.

## 2026-09-11 — v3.37: hook the real async stage loader and nativeize shadow flags

The v3.36 physical-Vita run advances past the previous JObj panic and reaches the retail GS_VS stage far enough to complete particle, joint-map, ground-parameter, collision and item normalization. The tail is `VITA_STAGE_NATIVE_PASS map_head=0x82cdadb8 maps=5 splines=1 shadows=6 internals=4 stage_params=10 raw=1`, followed by two successful particle-bank reuse markers, then a deliberate `HSD_PANIC ... ground.c:2526: 0`. The new coredump is an Undefined Instruction stop generated by the assert path with no data/prefetch fault address, so this is a deterministic stage-light/shadow invariant failure rather than another arbitrary ARM memory fault.

The most important v3.36 diagnostic is what is missing: there is no `VITA_STAGE_HSD_RAW_NATIVE_PASS`. The stage used by GS_VS is loaded asynchronously through `lbdvd` type 4 -> `grDatFiles_801C5FC0()`, which called `lbArchive_InitializeDAT()` directly and therefore bypassed `lbArchive`'s `PREPARE_RAW()` hook entirely. `grDatFiles_801C6478()` has the same direct-parse property for dynamically loaded stage archives. As a result, the pre-relocation HSD nativeizer implemented in v3.36 existed in the binary but never touched the actual match-stage DAT.

v3.37 hooks `mv_stage_archive_prepare_raw()` immediately before `lbArchive_InitializeDAT()` in both direct `grDatFiles` paths. Async retail stages identify themselves in the log as `file=grDatFiles_async`; dynamic stage archives use `file=grDatFiles_dynamic`. This preserves the intended order: validate/convert serialized HSD scalar descriptors while all DAT references are still offsets, then let the original HSD parser relocate the pointer fields. The downstream schema nativeizer still handles the non-HSD stage containers after relocation.

The v3.36 core also exposed a second genuine PPC/ARM layout mismatch in the six-entry `GroundShadowEntry` table. The two enabled serialized flags are stored as byte `0x80`, because the one-bit GameCube/PPC bitfield occupies the byte's MSB; the ARM build reads the same C bitfield from bit 0. v3.37 validates the relocated shadow table after `map_head->unk24` is normalized, converts only `0x80 -> 0x01`, accepts already-native `0x00/0x01`, rejects all other encodings, and emits `VITA_STAGE_SHADOW_NATIVE_PASS entries=... enabled=... converted=...`. The LightAnim pointers themselves remain untouched.

The `ground.c:2526` assertion is deliberately retained. Vita-only diagnostics now report the chosen stage lightset with `VITA_STAGE_LIGHT_SELECT ...`; if a LightAnim still cannot be found in the shadow table, the failure path emits `VITA_STAGE_SHADOW_LOOKUP_FAIL` followed by every `VITA_STAGE_SHADOW_ENTRY`. This avoids masking a real archive-selection or light-descriptor inconsistency while making the next hardware result directly actionable.

Runtime marker is `MELEE_VITA_GAME_BOOT v3.37`; VPK APP_VER is `00.47`. The full profile remains `MELEE_VITA_FULL_GAMEPLAY_SCENE=ON` and `MELEE_VITA_SKIP_CLASSIC_INTRO=ON` while the first real match remains the target.

The final source-checkout build completes through VELF/SELF/VPK. Hardware artifact: `build/vita-full/SmashMeleeVita-v3.37-stage-async-raw-shadow-light.vpk`, 3,178,895 bytes, SHA-256 `45762ba585ba4f0d9b57c3ff531e4ab70dfe4454507ae7158f1baf1fe3244741`; it is byte-for-byte identical to `SmashMeleeVita-assets.vpk`. `eboot.bin` is 2,748,128 bytes with SHA-256 `3982ff1126ca4e26d65431918a6d4c5d41a57d6ae862520ae10756649e317701`. The unstripped ELF is 9,159,472 bytes with SHA-256 `a6626aa6b75e9d3a0c2db6fb06604fb50e53ce413b1fe2944d302b4eb5c97975`, and contains the v3.37/upstream markers plus `VITA_STAGE_HSD_RAW_NATIVE_PASS`, `VITA_STAGE_SHADOW_NATIVE_PASS`, `VITA_STAGE_LIGHT_SELECT` and `VITA_STAGE_SHADOW_LOOKUP_FAIL`. The packaged SFO reports APP_VER `00.47`.

Physical-Vita target: repeat Classic -> select character -> START. The first required new marker is `VITA_STAGE_HSD_RAW_NATIVE_PASS file=grDatFiles_async ...`; after relocation the log should also show `VITA_STAGE_SHADOW_NATIVE_PASS entries=6 enabled=2 converted=2` for the stage captured in the v3.36 core. If the old `ground.c:2526` assert survives, the new light-selection and shadow-entry diagnostics should identify the exact mismatched LightAnim without requiring another speculative downstream patch.

## 2026-09-11 — v3.36: nativeize nested stage JObj/material graphs before pointer relocation

The v3.35 hardware run proves the bounded stage-DAT container nativeizer fixed the previous `Ground_801C34AC` Data Abort. The new stage reaches `VITA_STAGE_JOINT_MAP_NATIVE_PASS entries=1 pairs=22`, `VITA_COLL_NATIVE_PASS`, `VITA_STAGE_ITEM_NATIVE_PASS`, `VITA_STAGE_NATIVE_PASS`, and two successful `VITA_PS_BANK_NATIVE_REUSE` lines. It then stops deliberately in `jobj.c` with `load-only JObj requires rigid tree`, rather than suffering an arbitrary Data Abort.

The new core shows that panic is still the same mixed-endian archive class one level deeper, inside the HSD model graph. The active map JObj at `0x82c5a148` begins with serialized flag bytes `30 00 00 08`: GameCube/PPC correctly reads `0x30000008`, but the ARM runtime sees `0x08000030`. The latter falsely sets `JOBJ_PTCL`, so the Vita load-only guard rejects a perfectly ordinary rigid stage tree. Child/next/DObj pointer fields on the same descriptor are already valid Vita addresses because `HSD_ArchiveParse` relocated them, proving that the old architecture was still producing mixed native pointers plus big-endian HSD scalars.

v3.36 moves HSD model conversion to the only safe general boundary: the raw DAT hook immediately before `HSD_ArchiveParse`. `mv_stage_archive_prepare_raw()` detects archives with a `map_head`, validates every map-entry HSD root with the existing typed `mv_hsd_native_build_at()` walker while the source is still fully big-endian, then converts the supported descriptor graph in place without touching any relocation field. The walker covers JObj flags/transforms/matrices, DObj links, MObj render modes/material floats, PObj flags/counts/envelope weights, vertex-descriptor enums/strides, TObj enums/transforms/blending, image/TLUT/LOD metadata and TEV active flags. Texture bytes, display lists, vertex payloads, colors, packed PE state and every pointer field remain untouched. `HSD_ArchiveParse` then performs the normal pointer relocation on the already-native scalar graph, preserving object identity required by `Ground_801C34AC` and envelope reference resolution.

The pre-pass is fail-closed. Actual unsupported HSD features are still rejected by the existing typed validator instead of disabling `MELEE_VITA_HSD_LOAD_ONLY`, and malformed/unterminated descriptor tables produce a `VITA_STAGE_HSD_RAW_*` diagnostic before retail code can dereference them. A successful stage emits `VITA_STAGE_HSD_RAW_NATIVE_PASS ...` with unique descriptor counts. This extends the v3.35 schema-driven rule from stage containers to their nested HSD model graphs and avoids another sequence of downstream one-field byteswap fixes.

Runtime marker is `MELEE_VITA_GAME_BOOT v3.36`; VPK APP_VER is `00.46`. The full profile remains `MELEE_VITA_FULL_GAMEPLAY_SCENE=ON` and `MELEE_VITA_SKIP_CLASSIC_INTRO=ON` while the first real match is the active hardware target.

The final source-checkout build completes through VELF/SELF/VPK. Hardware artifact: `build/vita-full/SmashMeleeVita-v3.36-stage-hsd-preparse-nativeizer.vpk`, 3,177,752 bytes, SHA-256 `78d440903b7bbda1dc6bdb57e43099cc148bb38999c573bb7443d3f361b0db95`; it is byte-for-byte identical to `SmashMeleeVita-assets.vpk`. `eboot.bin` is 2,746,986 bytes with SHA-256 `131b60afd38196cd891870b76441b6eb116f3314a9f9ce96080807494a1af0ba`. The unstripped ELF is 9,158,948 bytes, SHA-256 `91d063dd850af0ba2cf4941d035e62b137fdea3b93c46cfbd8b2f9e532feaa8d`, and contains the v3.36 marker, upstream base marker, `VITA_STAGE_HSD_RAW_NATIVE_PASS` and fail-closed validation markers.

Physical-Vita target: repeat Classic -> select character -> START. Before the existing post-stage markers, a supported stage should now emit `VITA_STAGE_HSD_RAW_NATIVE_PASS file=... maps=... roots=...`. The old `jobj.c:633: load-only JObj requires rigid tree` panic caused by the byte-swapped `0x30000008 -> 0x08000030` flags must disappear. If the pre-pass instead reports `VITA_STAGE_HSD_RAW_VALIDATE_FAIL`, that identifies a genuinely unsupported HSD feature and should be implemented at the typed HSD boundary rather than bypassing the load-only guard.

## 2026-09-11 — v3.35: replace crash-by-crash scalar swaps with bounded stage-DAT nativeization

The v3.34 physical-Vita run proves the previous particle-bank corruption is fixed: the stage bank emits `VITA_PS_BANK_NATIVE_PASS`, the collision/item/stage normalizers all pass, and the same bank subsequently emits `VITA_PS_BANK_NATIVE_REUSE` twice without an `OS_PANIC`. The new failure is a main-thread Data Abort at runtime PC `0x810af21e` / DFAR `0x8a021000`. With the v3.34 runtime RX base at `0x8103b000`, the fault maps to linked `0x8107421e`, inside `Ground_801C34AC+0x59`, after the first real `GS_VS` stage has entered its JObj mapping path.

Core memory establishes that this is not an unrelated crash. The active `map_head` is `0x82c5bcec`; its relocated pointer fields are already valid native Vita addresses, but `map_head->unk4` is still stored as bytes `00 00 00 07`, observed by ARM as `0x07000000`. `Ground_801C34AC` consumes that field as the number of records in `map_head->unk0`. The seven real records are present and their relocated `joint`/`pairs` pointers are valid, but their `pair_count` values are likewise still serialized big-endian (`0x11000000`, `0x11000000`, `0x0f000000`, `0x15000000`, `0x11000000`, `0x0f000000`, `0x0e000000`), i.e. 17/17/15/21/17/15/14. Their pointed `(jobj_index, stage_info.x280_slot)` s16 pairs are also big-endian. This is the same mixed-endian archive condition responsible for v3.31-v3.33: HSD relocation fixes pointers, but ordinary numeric payloads remain in GameCube byte order.

v3.35 changes the strategy from isolated field patches to a bounded, schema-driven stage DAT nativeizer. `vita/stage_archive_vita.c` now derives each public root's true serialized span from `HSD_Archive::public_info` instead of trusting `sizeof(C_struct)`. A root conversion is rejected if the named public symbol does not resolve to the expected pointer or if the next public root leaves insufficient room. This rule is now used for `map_head`, `grGroundParam`, `coll_data`, and `itemdata`, and emits `VITA_DAT_ROOT_SPAN ...` diagnostics. It generalizes the lesson from v3.34: inferred/decomp-only tail fields are never allowed to cross into the next serialized root.

The `map_head` converter is also made schema-complete for the JObj mapping data exercised by retail `Ground_801C34AC`. It normalizes `map_head->unk4`, validates the relocated `unk0` record array, normalizes every record's `pair_count`, converts every pointed pair of s16 indices, validates traversal targets and the destination slot against `stage_info.x280[261]`, and emits `VITA_STAGE_JOINT_MAP_NATIVE_PASS entries=... pairs=...`. The converter is idempotent: already-native counts are accepted and pair arrays are only swapped when their owning serialized count is detected as raw big-endian.

This is the practical general solution for the port. A blind whole-DAT byteswap is unsafe because relocation metadata identifies pointer fields but carries no type information for ints, floats, enums, indices or byte streams. Instead, archives are nativeized once, by named public-root schema, immediately after HSD pointer relocation and before retail code consumes them. Public-symbol spans provide serialized boundaries; typed walkers convert all scalar descendants for each known root; opaque texture/display-list/byte data is left untouched; and validation fails closed with `VITA_DAT_*`/`VITA_STAGE_*` diagnostics rather than allowing an arbitrary Data Abort. Future mixed-endian failures should extend one schema at its nativeization boundary, not add swaps at the eventual crash site.

Runtime marker is `MELEE_VITA_GAME_BOOT v3.35`; VPK APP_VER is `00.45`. The full gameplay profile remains `MELEE_VITA_FULL_GAMEPLAY_SCENE=ON` with `MELEE_VITA_SKIP_CLASSIC_INTRO=ON` while the first real match is the active target.

The complete source-checkout build succeeds through VELF/SELF/VPK. Final hardware artifact: `build/vita-full/SmashMeleeVita-v3.35-stage-dat-nativeizer.vpk`, 3,173,800 bytes, SHA-256 `d12e1b9ab865d0f7aa235c6220060a073165339e54a7002e558a8c75065c1021`; it is byte-for-byte identical to the generated `SmashMeleeVita-assets.vpk`. `eboot.bin` is 2,742,871 bytes with SHA-256 `a64ba7fa69b254e07493133887114d9eb128672948240b20e7ddc19cc687ff1c`. The unstripped ELF contains the v3.35 boot marker plus `VITA_DAT_ROOT_SPAN`, `VITA_STAGE_JOINT_MAP_NATIVE_PASS`, `VITA_COLL_NATIVE_PASS`, `VITA_STAGE_ITEM_NATIVE_PASS`, and `VITA_STAGE_NATIVE_PASS`; `git diff --check` passes.

Physical-Vita target: repeat Classic -> select character -> START. Before the former `Ground_801C34AC` fault, the log should now show `VITA_DAT_ROOT_SPAN symbol=map_head ...` and `VITA_STAGE_JOINT_MAP_NATIVE_PASS entries=7 pairs=116 ...` for the stage captured in the v3.34 core. A later crash should be classified first by whether it consumes another not-yet-nativeized public-root schema; it should no longer be patched at the downstream faulting function when the source is serialized DAT state.

## 2026-09-11 — v3.34 physical-Vita crash: collision normalizer was overwriting `map_ptcl`

The v3.33 physical-Vita run proves the collision, stage-item and top-level stage normalizers all complete successfully: the log reaches `VITA_COLL_NATIVE_PASS`, `VITA_STAGE_ITEM_NATIVE_PASS` and `VITA_STAGE_NATIVE_PASS`. Immediately afterwards, however, the same stage particle command bank that had already emitted `VITA_PS_BANK_NATIVE_PASS ... version=66` fails a second locate with `VITA_PS_BANK_FAIL reason=version raw=001e native=1e00`, ending in the explicit `unsupported particle bank version` panic.

The addresses expose the corruption directly. For this stage `coll_data=0x82b92d94` and `map_ptcl=0x82b92dc0`, an exact delta of `0x2C`. The decomp declares `MapCollData::x2C` at that offset but marks it `/* inferred */`; the v3.32 collision converter nevertheless treated it as serialized collision data and executed `stage_swap32(&coll->x2C)`. The already-normalized particle header at `map_ptcl` starts with native bytes `42 00 1e 00`; that collision swap rewrote the same four bytes to `00 1e 00 42`, which exactly explains the later particle diagnostic (`raw=001e`, `native=1e00`). `mpLibLoad` consumes the collision payload only through `joint_count` at `+0x28`, so the inferred tail must not be touched for stage DAT archives.

v3.34 removes the `x2C` swap and constrains the Vita collision archive range check to `offsetof(MapCollData, x2C)`, i.e. the real `0x2C`-byte payload ending immediately before the aliased `map_ptcl` symbol. The particle locator also emits `VITA_PS_BANK_NATIVE_REUSE ...` when a previously-normalized bank is encountered; this does not change the loader semantics, but makes the expected second idempotent locate explicit in hardware logs. Runtime marker is `MELEE_VITA_GAME_BOOT v3.34` and VPK APP_VER is `00.44`.

The full gameplay profile remains `MELEE_VITA_FULL_GAMEPLAY_SCENE=ON` with `MELEE_VITA_SKIP_CLASSIC_INTRO=ON`. A full source-checkout build completes through VELF/SELF/VPK. Final hardware artifact: `build/vita-full/SmashMeleeVita-v3.34-collision-tail-particle-reuse.vpk`, 3,172,165 bytes, SHA-256 `8e4ed2dc2200da476fdbcf135ff71762df93bd7ed31d677c2d9abf01e2b1c3eb`; it is byte-for-byte identical to `SmashMeleeVita-assets.vpk`. `eboot.bin` is 2,741,224 bytes with SHA-256 `e36efca9277569254dae5bbf435e4aaede3d931b42201b42b83706017f17f993`. The unstripped ELF contains `MELEE_VITA_GAME_BOOT v3.34`, `MELEE_VITA_UPSTREAM_BASE 480b04454`, `VITA_PS_BANK_NATIVE_REUSE`, `VITA_COLL_NATIVE_PASS`, `VITA_STAGE_ITEM_NATIVE_PASS`, and `VITA_STAGE_NATIVE_PASS`; `git diff --check` passes.

Physical-Vita target: repeat Classic -> select character -> START. Around the previous failure point the expected order is the first `VITA_PS_BANK_NATIVE_PASS ... version=66`, then `VITA_COLL_NATIVE_PASS`, `VITA_STAGE_ITEM_NATIVE_PASS`, `VITA_STAGE_NATIVE_PASS`, and finally `VITA_PS_BANK_NATIVE_REUSE ... version=66`. `VITA_PS_BANK_FAIL reason=version raw=001e native=1e00` must no longer appear. Any later crash is the next genuine `GS_VS` initialization frontier.

## 2026-09-11 — v3.33 physical-Vita crash: stage item kind endian normalization

The v3.32 physical-Vita run proves both previous stage normalizers are now effective: the log reaches the first retail `GS_VS` and emits both `VITA_COLL_NATIVE_PASS` and `VITA_STAGE_NATIVE_PASS`, so the former collision-array overrun is no longer the active failure. The new core is again a main-thread Data Abort, this time at runtime PC `0x811a3242` with DFAR `0x2958fff0`. Compensating the `+0x7b000` runtime relocation maps the fault to linked address `0x81128242`, i.e. `it_8026B40C+0x9`.

`it_8026B40C` stores a stage `Article*` into `it_804A0F60[kind - It_Kind_Old_Kuri]`. Its caller in `Ground_801C2ED0` obtains `kind` directly from `stage_info.itemdata[i]->unk0`. The failure therefore moves the mixed-endian frontier one structure later: HSD has already relocated the `itemdata` pointer table and `GroundItemData*` entries, while the serialized `unk0` item-kind scalar is still GameCube big-endian. Consuming that raw scalar as native ARM produces an out-of-range stage-item table index and the observed Data Abort.

v3.33 extends `mv_stage_archive_prepare()` to receive `stage_info.itemdata` and normalize each `GroundItemData::unk0` before `Ground_801C2ED0` can register stage articles. The converter is range-checked against the archive, bounded to 64 table entries, requires a NULL terminator, accepts already-native values idempotently, and only swaps values that become a valid stage-item `ItemKind` from `It_Kind_Old_Kuri` through `It_Kind_Kyasarin_Egg`. Invalid values fail closed with `VITA_STAGE_ITEM_KIND_INVALID`; successful conversion emits `VITA_STAGE_ITEM_KIND_NATIVE ...` followed by `VITA_STAGE_ITEM_NATIVE_PASS ...`. Relocated `Article*` pointers are deliberately left untouched.

The full gameplay profile remains `MELEE_VITA_FULL_GAMEPLAY_SCENE=ON` with the temporary `MELEE_VITA_SKIP_CLASSIC_INTRO=ON` match-focus checkpoint. The v3.33 source compiles, links and packages successfully through VELF/SELF/VPK. Final hardware artifact built directly in the source checkout: `build/vita-full/SmashMeleeVita-v3.33-stage-item-endian-gsvs.vpk`, 3,172,420 bytes, SHA-256 `317567f9f6e5f7d0bc2aa8a4092d46031cb83e4b97474cdeb8a23e1caf3d59e2`; it is byte-for-byte identical to `SmashMeleeVita-assets.vpk`. `eboot.bin` is 2,741,335 bytes with SHA-256 `202cf31eb3d8b41c99bd4868dfb7e29c0824358eee862ef889e7bd565eab624b`. The unstripped ELF contains `MELEE_VITA_GAME_BOOT v3.33`, `MELEE_VITA_UPSTREAM_BASE 480b04454`, `VITA_COLL_NATIVE_PASS`, `VITA_STAGE_ITEM_KIND_NATIVE`, `VITA_STAGE_ITEM_NATIVE_PASS`, and `VITA_STAGE_NATIVE_PASS`; VPK metadata is APP_VER `00.43`.

Physical-Vita target: repeat Classic -> select character -> START. Expected progression is `VITA_COLL_NATIVE_PASS`, one or more `VITA_STAGE_ITEM_KIND_NATIVE ...` markers where needed, `VITA_STAGE_ITEM_NATIVE_PASS ...`, then `VITA_STAGE_NATIVE_PASS`. A crash after those markers is the next genuine fighter/item/model/runtime frontier and should be diagnosed from a fresh log/core rather than reopening the fixed collision or stage-item paths.

## 2026-09-11 — v3.32 physical-Vita crash: GS_VS collision data endian normalization

The v3.31 physical-Vita run proves the previous stage archive fix is effective. The log reaches the original first-match `GS_VS`, emits `VITA_STAGE_NATIVE_PASS map_head=... maps=6 ... internals=20 stage_params=8 raw=1`, and therefore gets past the former `grDatFiles_801C6228` overrun. The next failure occurs later in the same stage bootstrap, while the retail collision subsystem is loading `stage_info.coll_data`.

The new core records main-thread runtime PC `0x811138b6` with the runtime `melee_vita` RX base at `0x8107b000`. Relocation compensation maps the fault to linked address `0x810988b6`, i.e. `mpLibLoad+0xDE`. The stop reason is a Data Abort and DFAR is `0x89d00004`. Core registers/memory show the active collision-joint walk has just advanced to the end of an allocated region (a preceding record at `0x89cfffcc` reaches `0x89d00000` with the 0x34-byte runtime joint stride), so the old code walks beyond the real joint array.

The root cause is the same mixed archive state seen in earlier Vita frontiers: HSD relocation has already converted `MapCollData` pointers to native addresses, but the collision header and pointed numeric records are still serialized GameCube big-endian. In particular `vert_count`, `line_count`, `joint_count`, the global line spans, every `MapLine`, every `MapJoint`, and collision vertex floats are consumed directly by `mpLibLoad`. Treating those scalar fields as native ARM lets the joint loop and later line/vertex loops run beyond their arrays.

v3.32 extends `vita/stage_archive_vita.c` instead of bypassing collision setup. Before `mpLibLoad` can observe the stage data, `mv_stage_archive_prepare` now also receives `stage_info.coll_data` and performs one-shot BE-to-ARM normalization. It converts collision counts and spans, all vertex coordinates, all `MapLine` indices/flags/neighbors, all `MapJoint` line spans/bounds/vertex spans, and `x2C`. Relocated pointers are deliberately preserved. Sanity limits match the actual runtime allocations in `mplib.c` (2048 vertices, 1536 lines, 256 joints), and every archive range, line span, vertex index and joint vertex span is validated before use. Successful conversion emits `VITA_COLL_NATIVE_PASS ...`; malformed data fails closed with a specific `VITA_COLL_*_INVALID` marker rather than producing an arbitrary Data Abort.

The full Vita profile still uses `MELEE_VITA_FULL_GAMEPLAY_SCENE=ON` and the temporary `MELEE_VITA_SKIP_CLASSIC_INTRO=ON` match-focus checkpoint. The v3.32 source compiles and links successfully through VELF/SELF/VPK after the collision normalizer was added. This checkpoint is not claimed playable until physical hardware proves that `VITA_COLL_NATIVE_PASS` is followed by further `GS_VS` initialization.

Final v3.32 verification: hardware artifact `build/vita-full/SmashMeleeVita-v3.32-collision-endian-gsvs.vpk` is 3,171,868 bytes with SHA-256 `ea46cc00d59c8f987e203bbd885550a15d545b8f10c2594612f4117bf8c821d1`; it is byte-for-byte identical to the generic `SmashMeleeVita-assets.vpk`. Packaged `eboot.bin` is 2,740,682 bytes with SHA-256 `0d61674b959785a0a448b33eca5e984de2f3857bb79e567ba606b7d66f9a4959`. The unstripped VELF contains `MELEE_VITA_GAME_BOOT v3.32`, `MELEE_VITA_UPSTREAM_BASE 480b04454`, `VITA_STAGE_NATIVE_PASS`, and `VITA_COLL_NATIVE_PASS`; the VPK metadata is APP_VER `00.42`. `git diff --check` passes and the final full build exits successfully through VELF, SELF and VPK.

Physical-Vita target: repeat Classic -> select character -> START. The expected new marker after `VITA_STAGE_NATIVE_PASS` is `VITA_COLL_NATIVE_PASS ...`. A crash after that line should be treated as the next real `GS_VS` model/fighter/item/runtime frontier rather than the previous collision-array overrun.

## 2026-09-11 — v3.31 physical-Vita crash: GS_VS map_head scalar endian normalization

The v3.30 hardware run proves the mode routing, fighter-layout fix and temporary Classic Intro bypass are all working. The log reaches `GAME_1P_RETAIL_CONTINUE_BEGIN`, emits `VITA_CLASSIC_INTRO_BYPASS ... next=GS_VS`, initializes the common/menu effect banks, and then enters the first original `gm_Scene_Vs_OnEnter`. The new failure is therefore inside the real first-match stage bootstrap rather than the Classic presentation path.

The supplied core maps `melee_vita` at runtime text base `0x81019000`, a `+0x19000` relocation from the linked image. Main-thread runtime `PC=0x810904aa` / `LR=0x810905a3` therefore resolve to linked `0x810774aa` / `0x810775a3`, i.e. `grDatFiles_801C6228+0x16` called from `grDatFiles_801C6038+0xab`. The recovered stack also reaches `Ground_801C0754 -> Stage_802251E8 -> gm_Scene_Vs_OnEnter`, proving the crash is the first GS_VS stage DAT initialization.

Core memory shows the same mixed relocation/endian pattern previously seen in particle/camera data: archive pointers have already been relocated to native Vita addresses, but scalar fields remain serialized GameCube big-endian. Example count words appear as `0x07000000`, `0x02000000`, `0x24000000` while their underlying bytes encode 7, 2 and 36. `grDatFiles_801C6228` trusted `UnkStageDat::unk2C` directly, overran the relocated `unk28` pointer table and eventually dereferenced garbage `0x9ab482fc`.

v3.31 adds `vita/stage_archive_vita.c` and calls `mv_stage_archive_prepare()` immediately after `map_head`/`grGroundParam` are resolved, before particle setup and before `grDatFiles_801C6228`. The normalizer is range-checked and idempotent. It converts the top-level `UnkStageDat` counts (`unkC`, `unk14`, `unk1C`, `unk24`, `unk2C`), per-map scalar counts (`unk24`, `x30`), embedded `GrJoint` s16 triplets, `UnkStageDatInternal::unk4` flags, the `GroundParam` numeric fields and every `StageParam` row. Relocated pointers, display lists, texture data and byte streams are deliberately left untouched. Successful conversion emits `VITA_STAGE_NATIVE_PASS ...`.

The source is now also genuinely merged with doldecomp upstream through `480b04454`: merge commit `c4324eb51` has parents `59be22d36` and `480b04454`, and local Git reports 0 commits behind upstream. This v3.31 work is intentionally left uncommitted pending physical-Vita validation.


Final v3.31 verification: full gameplay remains ON and the temporary Classic Intro visual bypass remains ON. The build completes through VELF, SELF and VPK with exit 0. `vita/tools/test_menu_boundary.py` passes both suites (45 GM IDs; 51 retail Event entries). Hardware artifact `build/vita-full/SmashMeleeVita-v3.31-stage-endian-gsvs.vpk` is 3,170,326 bytes with SHA-256 `fcc8de24825330e2b2a089191d93cdb4ec6cc363ae60a14297996ae6e3d43223`; it is byte-for-byte identical to `SmashMeleeVita-assets.vpk` and has ZIP/VPK magic `PK\x03\x04`. Packaged `eboot.bin` is 2,739,470 bytes with SHA-256 `c41d290de1fe21c3857d14ac92709c2f725ecafa2513da1bea5484e23bd58dce` and SELF magic `SCE\0`. The unstripped executable contains `MELEE_VITA_GAME_BOOT v3.31`, `MELEE_VITA_UPSTREAM_BASE 480b04454`, `VITA_STAGE_NATIVE_PASS`, and `VITA_CLASSIC_INTRO_BYPASS`; the generated SFO contains APP_VER `00.41`.

Physical-Vita validation target: repeat Classic -> select character -> START. The old `grDatFiles_801C6228` overrun should now be preceded by `VITA_STAGE_NATIVE_PASS` and no longer fault at the previous PC. A further crash after that marker should be treated as the next real GS_VS stage/model/collision/fighter frontier rather than evidence that the match is already playable.


## 2026-09-11 — v3.30 selective doldecomp upstream alignment through 480b04454

The Vita source has been selectively aligned with all 12 doldecomp/melee commits after the previous common base `e3fff7e9c` through current `upstream/master` `480b04454` (`Cleanup lb_0219 with proper allocation, function, and variable naming (#3444)`). This was deliberately not done with a blind `git pull` or history rewrite: the working tree contains extensive Vita-specific changes, so each upstream patch was applied/fused into the existing local source while preserving all `MELEE_VITA_PLATFORM` and full-gameplay hooks. No commit, push, reset or clean was performed. Git ancestry therefore still reflects the local checkpoint history; this section records source-level alignment, not a merge commit. Runtime logs identify this checkpoint with `MELEE_VITA_UPSTREAM_BASE 480b04454`.

The integrated correctness changes are directly relevant to the ARM port: CObj top-half projection storage now uses the complete projection matrix; the upstream `lbspdisplay` out-of-bounds color accesses are removed; `mplib` pointer differences use pointer-width arithmetic and `sizeof(CollLine)`; and the fake aggregate `HSD_GObj_Entities` assumption is replaced by the actual `HSD_GObjPLinkHead`/named GObj APIs. All Vita wrappers were updated to the same named GObj API (`HSD_GObjSetInitDefaults`, `HSD_GObjInit`, `HSD_GObj_RunProcs`). The remaining upstream cleanup/naming commits for gm, Crazy Hand, debug symbols, crowd SFX and `lb_0219` were also integrated.

The upstream `mncharsel` cleanup was fused with the Vita native CSS converter rather than reverting it. The converter now feeds the typed `MnSelectChrDataTable` / `MnSelectChrModels` structures directly; camera, light/fog descriptors and the background model use the typed fields instead of the old `+0x10`/`MODELS`/`ANIM` assumptions. The upstream `gm_1A45 -> gmscene` rename is complete in source and Vita CMake; the local retail-scene completion/current-exit-data hooks remain in `gmscene.c`. The ARM-visible `gm_GetDbPauseFlag` definition is kept as `bool` to match the updated declaration.

Validation was performed incrementally after each upstream group. The final full profile is still configured with `MELEE_VITA_FULL_GAMEPLAY_SCENE=ON` and the temporary `MELEE_VITA_SKIP_CLASSIC_INTRO=ON` match-focus checkpoint. `git diff --check` passes, there are no remaining `.rej` files or legacy `gm_1A45` / `HSD_GObj_Entities` references in source/Vita code, and `vita/tools/test_menu_boundary.py` passes both suites (all 45 GM IDs and all 51 retail Event entries). The full ARM build links and packages after every integration group. Final v3.30 verification: named hardware artifact `build/vita-full/SmashMeleeVita-v3.30-upstream-480b04454-gsvs.vpk`, 3,169,311 bytes, SHA-256 `ddda47cdd34c40733e133b566b9abd5b282db4d84d391bc255b7f2c83da5d9db`. It is byte-for-byte identical to `SmashMeleeVita-assets.vpk`; VPK magic is `PK\x03\x04`. Packaged `eboot.bin` is 2,738,450 bytes, SHA-256 `8d891a90d44365d7b86dcb6d97bac66de0fb6c9a847246ec5a756d1c3204b38c`, with SELF magic `SCE\0`. The unstripped executable contains `MELEE_VITA_GAME_BOOT v3.30`, `MELEE_VITA_UPSTREAM_BASE 480b04454`, `VITA_FTDATA_LAYOUT_SAFE_PASS`, and `VITA_CLASSIC_INTRO_BYPASS`; the generated SFO contains APP_VER `00.40`.

Hardware target remains unchanged: Classic CSS START should reach the retail continuation, emit `VITA_FTDATA_LAYOUT_SAFE_PASS`, bypass only the visual `GS_INTRO_EASY` presentation via `VITA_CLASSIC_INTRO_BYPASS`, and then enter the first original `GS_VS`. The upstream sync reduces known UB/layout hazards but is not claimed to make the match playable until tested on physical Vita.

## 2026-09-11 — v3.29 physical-Vita crash: DOL-adjacent fighter tables corrupt effect metadata

Fresh v3.28 physical-Vita evidence proves the CSS routing correction is working. START exits Character Select with `pending=1` and reaches `GAME_1P_RETAIL_CONTINUE_BEGIN mode=CLASSIC(3) ... state=112`; the existing particle-bank, Intro asset proxy and CObj endian conversions all execute before the new crash. The failure is therefore later in the retail Classic Intro fighter-demo path, not a return-to-menu regression.

The supplied ARM core records main-thread `PC=0x8137fb2a`, `LR=0x8126e6bd` with runtime text relocation `+0x6e000`. After relocation compensation these resolve to `strcmp` and `HSD_ArchiveGetPublicAddress`. At the fault, one strcmp operand points to the valid archive public symbol `effMarioDataTable` while the other operand is NULL. Stack recovery reaches `efAsync_LoadSync -> Fighter_UnkInitLoad_80068914 -> ftDemo_CreateFighter -> Player_80036F34 -> gm_Scene_IntroEasy_OnEnter`, so Mario effect metadata is corrupted before the demo fighter is instantiated.

The root cause is a retained GameCube DOL layout assumption in `ft_800852B0`. The original binary deliberately places `ftData_Table_Unk0` immediately after `CostumeListsForeachCharacter`, places `ftData_UnkIntPairs` at costume-base + 5940, and places `ft_8045993C` immediately after `gFtDataList`. The Vita build uses `-fdata-sections`, so those objects are independent linker sections and are not adjacent. The old pointer arithmetic therefore wrote into unrelated live objects.

The current Vita link map proves the exact overwrite. `efAsync_DatEntries` begins immediately after `CostumeListsForeachCharacter`; the old `unk0[i].data = NULL` at `i=2` lands on `efAsync_DatEntries[1].effDataTable_name`, turning the Mario root name into NULL exactly as observed in the core. The second derived pointer falls inside the Crazy Hand motion-state table, creating a latent fighter-data corruption. The old `gFtDataList + Ft_Kind_Max` assumption points into `efAsync_AllocData`, corrupting the effect allocator as well. These are three concrete memory overwrites from one initialization routine.

v3.29 preserves the original non-Vita implementation but uses the real named symbols on Vita: `ftData_Table_Unk0`, `ftData_UnkIntPairs`, and `ft_8045993C`. `efAsync_LoadSync` additionally logs `VITA_EF_LOAD_SYNC ...` and fails closed on a NULL effect root rather than passing it into strcmp. `ft_800852B0` emits `VITA_FTDATA_LAYOUT_SAFE_PASS ...` after completing the layout-safe reset.

To move hardware testing onto the actual match instead of spending further iterations on the Classic splash/demo fighter, v3.29 adds the temporary full-profile option `MELEE_VITA_SKIP_CLASSIC_INTRO=ON`. The original Classic state-0 `on_enter` still runs first and therefore prepares matchup data, GameCache, stage kind, fighter preload and audio exactly through retail code. Only the `GS_INTRO_EASY` presentation scene enter/frame/exit is bypassed; the state machine then advances to the original first `GS_VS`. The log marker is `VITA_CLASSIC_INTRO_BYPASS state=0 scene=GS_INTRO_EASY next=GS_VS`. This bypass is a development checkpoint and should be removed after the real match path is stable.

Final v3.29 build verification: `build/vita-full` is configured with `MELEE_VITA_FULL_GAMEPLAY_SCENE:BOOL=ON` and `MELEE_VITA_SKIP_CLASSIC_INTRO:BOOL=ON`, and compiles/links/packages through VELF, SELF and VPK with exit 0. Named hardware artifact: `build/vita-full/SmashMeleeVita-v3.29-ftdata-layout-gsvs.vpk`, 3,169,446 bytes, SHA-256 `93267c1c70d0cb778f7686e72b04972b8409ba6eeb351397648520b451d4e8fe`. `eboot.bin` is 2,738,482 bytes, SHA-256 `86847752a1aca6ecc1dd515c8d2d4625c237d58b7b4c91f68c1533e881659b52`. VPK magic is `PK\x03\x04`, SELF magic is `SCE\0`, the ELF contains `MELEE_VITA_GAME_BOOT v3.29`, `VITA_FTDATA_LAYOUT_SAFE_PASS`, `VITA_EF_LOAD_SYNC`, and `VITA_CLASSIC_INTRO_BYPASS`, and the generated SFO contains version `00.39`.

Hardware test target: 1-P -> Regular Match -> Classic -> select a character -> START. Expected progression is layout-safe fighter init, CSS `pending=1`, retail continuation, the Classic Intro bypass marker, then entry into the first real `GS_VS`. If it still crashes, the next log/core should now identify a genuine stage/fighter/item/gameplay initialization frontier rather than the three fixed adjacent-section corruptions.

## 2026-09-11 — v3.28 fix: START from CSS must continue into the retail 1P graph

Physical-Vita feedback on v3.27 shows that pressing START after a valid character selection returns to MAIN instead of advancing into the Classic game flow. Source inspection confirms that the original CSS logic is correct: START sets `mnCharSel_804D6CF6 = 1`, while value 2 is reserved for BACK. `gmClassic_801B3E44` likewise treats only `pending_scene_change == 2` as a return to `GM_MENU`; a normal START stores the selected fighter data and sets the next Classic state.

The regression was packaging/profile related. v3.27 was built from `build/vita`, whose CMake cache still had `MELEE_VITA_FULL_GAMEPLAY_SCENE=OFF`. In that checkpoint profile `mv_onep_mode_run` stops after preparing the post-CSS state and returns success; the outer loop then unconditionally assigned `pending_mode = GM_MENU`. This exactly explains the observed behavior and is independent of the CSS START input itself.

v3.28 makes the full retail gameplay profile the default. `Makefile.vita` now passes `-DMELEE_VITA_FULL_GAMEPLAY_SCENE=ON` unless explicitly overridden with `FULL_GAMEPLAY=OFF`, and the CMake option default is also ON. For Classic/Adventure, the full continuation now returns the actual pending GameMode produced by `mv_gm_vita_continue_mode`; `main.c` consumes that destination instead of hard-coding `GM_MENU`. Negative runner results stop the dispatcher and are logged as errors rather than silently reopening MAIN.

The v3.27 SIS work is retained in this full profile: original `SdMenu`/`SdSlChr` text contexts are captured into the GX command stream, per-command projection and the glyph alpha test are replayed through vitaGL, and the Classic Intro JObj endian frontier remains adapted. Therefore the next physical test must use the full v3.28 package, not the earlier 1.6 MB frontier build.

Expected forward-path evidence after START is `GAME_1P_CSS_EXIT ... pending=1`, followed by `GAME_1P_RETAIL_CONTINUE_BEGIN` and entry into the next retail Classic state/Intro rather than `GAME_MODE_RETURN ... destination=1` immediately after the CSS. Any new crash beyond that point is a later gameplay/asset frontier and should be debugged from the fresh log/core without reverting the routing fix.

v3.28 full build verification: `build/vita-full` configures with `MELEE_VITA_FULL_GAMEPLAY_SCENE:BOOL=ON`, compiles/links/packages successfully through VELF, SELF and VPK, and `git diff --check` passes for the routing/version/status changes. Physical-Vita artifact: `build/vita-full/SmashMeleeVita-v3.28-full-routing.vpk`, 3,169,023 bytes, SHA-256 `300754daed52c77908bd6ecc79304cb7ad8d88df24255da7257ec5c4b46e9994`. VPK magic is `PK\x03\x04`; the unstripped executable contains `MELEE_VITA_GAME_BOOT v3.28` and the generated SFO contains version `00.38`.


## 2026-09-11 — v3.26 Classic camera endian + CSS animated TLUT/TEV correction

Fresh physical-Vita evidence advances beyond the v3.25 particle-bank failure. The log reaches `VITA_PS_BANK_NATIVE_PASS ... version=66 base=0 commands=592 tex_groups=36 form=0`, proving the v3.25 big-endian particle-bank conversion is executing successfully in the retail Classic continuation. The next failure moves forward to `HSD_PANIC .../cobj.c:1308: 0` during `gm_Scene_IntroEasy_OnEnter`: the Classic Intro camera descriptor is being loaded from `GmIntEz.dat` after archive pointer relocation, but its scalar payload is still GameCube big-endian.

v3.26 adds a Vita-only native normalization path in `cobj.c`. `CObjLoad` detects the characteristic byte-swapped projection enum, copies the descriptor, and converts flags, projection type, viewport, scissor, roll, near/far, perspective/frustum/ortho projection parameters, eye/interest WObj positions and up-vector floats to ARM-native representation. Pointer fields remain the already-relocated archive pointers. Native descriptors produced by the existing menu converters are left untouched. Successful retail conversion logs `VITA_COBJ_NATIVE_PASS projection=... viewport=... near=... far=...`. This is intended to fix the current Intro Easy panic without changing the non-Vita path.

The physical-Vita Character Select screenshot also exposes a separate renderer defect: static background/roster elements are broadly recognizable, while the large selected-character portrait, stock icons and other character-dependent textures use incorrect image/palette content compared with the retail reference. A static audit of `MnSlChr.usd` shows that this is not a missing multitexture problem: across all nine CSS sets, every material has at most one TObj. The important animated assets are palette-indexed. The main 1P portrait animation contains 118 paired TIMG/TLUT entries at 136x188 in CI8 format with 256-entry RGB5A3 palettes; the stock/icon animation contains up to 129 paired TIMG/TLUT entries in CI4 with 16-entry RGB5A3 palettes.

The concrete Vita bug was in `capture_material_state`: TIMG animation correctly changes `tobj->imagedesc`, but the capture path always copied the static `tobj->tlut`. Retail `HSD_TObjSetup` instead uses `tobj->tluttbl[tobj->tlut_no]` after a TCLT animation update. v3.26 now mirrors that behavior through a bounds-aware `capture_effective_tlut` helper for both primary and secondary texture capture, falling back to the static TLUT only when no animated palette is active. This should keep each CI4/CI8 character image paired with its corresponding animated palette.

The CSS material audit also found 22 unique custom TObj TEV descriptors. The active forms used by the character-select assets are single-texture HSD-generated graphs with color-only `0x400000..`, alpha-only `0x800000..`, combined `0xC00000..`, or inactive TEV. v3.26 extends the CPU TEV matcher/baker to the exact color and alpha interpolation graph observed in `MnSlChr.usd`, including the relevant TEV scale modes, while unrelated TEV graphs remain fail-closed. The CSS no longer initializes replay in the old relaxed-from-command-0 mode: it uses the strict replay path, so unsupported materials are skipped/logged rather than knowingly drawn with a false approximation. Expected CSS initialization is therefore `VITAGL_REPLAY_READY ... skipped=0`; any nonzero skipped count is now actionable renderer evidence.

The full gameplay profile continues to link the retail scene manager and Ground/Fighter/Item/HUD runtime with zero unresolved symbols. The v3.26 source checkpoint has compiled and packaged successfully after the camera, animated-TLUT, TEV and strict-CSS changes; final named artifact/hash verification is recorded below after the release build. Visual correctness is not claimed until the new package is tested on physical Vita.

Final v3.26 build/verification: `build/vita-full` compiles, links and packages with exit 0 and zero unresolved symbols. Named physical-Vita artifact: `build/vita-full/SmashMeleeVita-v3.26-css-tlut-camera.vpk`, 3,166,970 bytes, SHA-256 `8151720c20dba8a2aa361f0cb86ff1dca712399d6bef32e287f73cdc002c2f2b`. The packaged `eboot.bin` is 2,736,125 bytes, SHA-256 `6ea1b7c13ab75676d9018b7a1fff7437eb917882a7540c8bd74db67fef4132e4`. VPK magic is `PK\x03\x04`, SELF magic is `SCE\0`, and the unstripped executable contains the `MELEE_VITA_GAME_BOOT v3.26` banner. Existing enum-size linker warnings remain non-fatal.

Hardware test for v3.26: compare the same Classic CSS against the supplied retail reference, paying special attention to the 136x188 portrait and stock icons; then select a character and press START. The expected post-CSS progression is the existing particle conversion marker followed by `VITA_COBJ_NATIVE_PASS` and continued Intro Easy initialization. Return a fresh `runtime.log` and core if a new crash occurs.


## 2026-09-10 — v3.25 physical-Vita post-CSS crash / particle-bank native conversion

The latest physical-Vita full-runtime build proves that the former CSS routing bug is fixed: Classic exits Character Select with `pending=1`, reaches `GAME_1P_RETAIL_CONTINUE_BEGIN mode=CLASSIC(3) ... state=112 source=gm_1A3F.c`, and enters the retail Classic Intro Easy path. The crash therefore occurs after the correct forward transition, not in CSS input or the outer menu dispatcher.

The supplied core is a main-thread Data Abort. After compensating for the Vita module relocation, the faulting PC resolves to `psInitDataBankLocate` in `particle.c`; the stack also resolves through `efAsync_LoadSync(0) -> fn_80186634 -> gm_Scene_IntroEasy_OnEnter -> gm_801A4014 -> mv_gm_vita_continue_mode`. The fault address is `0x89c00000`. The loaded `EfCoData.dat` texture bank starts with raw bytes `00 00 00 24`, which is the GameCube big-endian value 36, but the old PPC-oriented code read it natively on ARM as `0x24000000` and then walked millions of bogus texture-group entries.

v3.25 makes the particle/effect data-bank path ARM-native while preserving the original non-Vita path. Under Vita, `psInitDataBankLocate` now recognizes raw v0/v0x40-v0x43 banks, converts command-bank IDs/counts and offset tables, relocates command pointers, converts every `HSD_PSCmdList` scalar/header field including IEEE-754 values, converts texture-group metadata and texture/palette pointer tables, and converts optional form-group tables. Bounds guards reject impossible command counts, group counts and offsets before any unbounded relocation loop. The conversion is idempotent for an already-native bank. `psInitDataBankLoad` also invokes the idempotent converter so async/preloaded load-only paths cannot bypass endian normalization.

Particle bytecode remains byte-oriented and is not rewritten. Its embedded float operands are now decoded explicitly from GameCube big-endian IEEE-754 bytes in `psReadFloat`; byte-sized opcodes and explicitly shifted BE16 operands retain the original interpreter behavior. Texture/palette payload bytes remain in their GameCube texture formats for the existing GX/vitaGL decoder. A successful conversion logs `VITA_PS_BANK_NATIVE_PASS ... version=66 ... commands=... tex_groups=...`.

Retail-data audit of the extracted GALE01 effect set found 27 `Ef*.dat` files with active particle sub-banks; all use command-bank version 0x42 and satisfy the v3.25 bounds/layout assumptions. Nine other effect DATs have both particle sub-bank pointers null and are skipped by the original `efAsync` condition. In particular, `EfCoData.dat / effCommonDataTable` contains base command ID 0, 592 command lists and 36 texture groups, matching the values observed around the crash.

The full-gameplay profile still links the retail scene manager, Classic/Adventure continuation and the large Ground/Fighter/Item/HUD runtime with zero unresolved symbols. THP paired-single PPC decoding and GameCube MCC/FIO host-debug paths remain explicit compatibility frontiers rather than fake implementations. The per-command GX capture/replay work and corrected LiveArea packaging remain in the same source tree.

Build/verification: the final `build/vita-full` compile/link/package completes with exit 0 and zero unresolved symbols; the existing enum-size linker warnings remain. The test artifact is `build/vita-full/SmashMeleeVita-v3.25-particle-endian.vpk`, 3,166,180 bytes, SHA-256 `9b47c1916756669b7e9d276984afdbced079d3681962018c5456a00e11786ef9`. The packaged `eboot.bin` is 2,735,139 bytes, SHA-256 `025d59b84484169044c4efe8da3ea15eff122b22f8809e25832bcf0aa23db796`. VPK magic is `PK\x03\x04`, SELF magic is `SCE\0`, and the unstripped executable contains the `MELEE_VITA_GAME_BOOT v3.25` banner. `git diff --check` is required to remain clean before handoff.

Hardware test for v3.25: Opening -> 1-P -> Regular Match -> Classic -> select a character -> START. The expected new marker immediately inside the former crash area is `VITA_PS_BANK_NATIVE_PASS` for bank 0, followed by continued Intro Easy initialization. Return the new `runtime.log` and a core only if a new crash occurs; the next failure, if any, should now expose the next genuine retail-runtime endian/lifecycle frontier rather than the old particle group-count corruption.

## 2026-09-10 — v3.23 physical-Vita CSS crash analysis / HSD heap lifetime fix

Fresh physical-Vita v3.22 evidence reaches Opening, original Title, MAIN, Regular Match routing and the Classic CSS transition. The last successful marker is `GAME_CSS_NATIVE_PREPARE_PASS source=MnSlChr.usd sets=9 camera=table+0 renderer=vitaGL`; the crash occurs before the first CSS capture/replay frame. The supplied core is a main-thread Data Abort (stop reason `0x30004`) with runtime PC `0x810BE3A2`, LR `0x810BE331`, DFSR `0xCF` and DFAR `0x00803F04`. Matching the tested 19:15 VELF, rather than the later-overwritten map, identifies the fault as `CreateGObj+0x9A` in `gobjplink.c`: `ldrb r2, [r1,#4]`. At the fault R1 is `0x00803F00`, so the p-link traversal is dereferencing a stale/non-native list head.

The root cause is the direct-scene GObj lifetime policy, not `MnSlChr` conversion or vitaGL. `HSD_GObj_80391304` allocates `HSD_GObj_Entities`, `plinklow_gobjs`, GX-link lists, proc lists and ObjAlloc slabs from `HSD_GetHeap()`. Every retail-style `lbDvd_80018CF4()` calls `lbHeap_80015900()`, which recreates that main heap through `HSD_CreateMainHeap()`. The old Vita one-shot `scene_objects_initialized` flag incorrectly reused those heap-owned control arrays after the heap had been destroyed and recreated. In the captured crash, p-link 3 had consequently become `0x803F00`, and the next priority insertion faulted at `0x803F04`.

v3.23 fixes the lifetime at the heap boundary. Under Vita, `HSD_CreateMainHeap()` now increments an HSD heap-generation counter exposed by `HSD_GetHeapGeneration()`. `mv_scene_vita_objects_init()` records the generation that owns the current GObj library: it reuses an empty library only while the generation is unchanged, and automatically runs the original `HSD_GObj_80391304` again after a preload-created heap generation change. This preserves valid same-heap reuse while making Title -> Menu -> CSS -> SSS heap transitions safe.

Additional hardware diagnostics now bracket the exact former crash region. `GAME_CSS_GOBJ_HEAP_SYNC` logs the heap generation plus `HSD_GObj_Entities`, p-link 3 head and low pointer before `mnCharSel_Scene_OnEnter`; `GAME_CSS_NATIVE_TABLE_READY`, `GAME_CSS_SIS_BEGIN/PASS` and `GAME_CSS_BUILD_BEGIN/PASS` isolate SIS loading and `mnCharSel_802640A0`. A successful run should proceed to the existing `GAME_CSS_CAPTURE_INIT_PASS` and then the CSS frame telemetry.

The SSS/third-state work is preserved. The default hardware-checkpoint profile runs the original CSS and SSS state machines and the original third-state `on_enter` that builds match data, then reports `GAMEPLAY_SCENE_FRONTIER_READY`. The deeper retail `gm_Scene_Vs_OnEnter` / `gm_Scene_Training_OnEnter` integration remains present behind CMake option `MELEE_VITA_FULL_GAMEPLAY_SCENE=ON`. That full profile currently exposes the expected large Ground/Fighter/Item/HUD/GX runtime link frontier; its source work was not discarded to make the checkpoint package.

The normal v3.23 build is the package intended for the next physical-Vita test. Test path: Opening -> START -> 1-P Mode -> Regular Match -> Classic. Verify that the CSS appears and accepts input; then select a character, proceed to Stage Select, select a stage, and return the fresh `runtime.log` plus a core only if a new crash occurs. No v3.23 hardware success is claimed until that test is performed.

Build/verification: `git diff --check` passes; `make -f Makefile.vita` completes VELF, SELF and VPK packaging. `build/vita/test-env/bin/python vita/tools/test_menu_boundary.py` still passes the 45-GM-ID storage/reset checks and all 51 retail Event records. Hardware checkpoint artifact: `build/vita/SmashMeleeVita-v3.23-hsd-heap-fix.vpk`, 1,616,016 bytes, SHA-256 `a4d30f4626992ae775f74bfd2b57403df1c594a067d0b1eba36c2f7510f105d3`. `eboot.bin` is 1,175,039 bytes, SHA-256 `d80b0917106d7474b4fdc58c836cc795b03bbde0fceb13a9698e846ec425cb24`. Build log: `build/vita/v3.23-build.log`. The known enum-size linker warnings remain.


## Active integration — supersedes v3.22 frontier policy

### 2026-09-10 active update - native SSS and third-state routing

The temporary Vita SSS stop has now been removed in source. A dedicated `MnSlMap.usd/.dat` converter builds the original `MnSelectStageDataTable` as ARM-native data: the perspective camera plus all 12 JObj/AnimJoint/MatAnim sets are converted, and inspection of the retail asset confirmed that all 12 ShapeAnim pointers are null. `mnstagesel.c` therefore runs its original HSD state machine without requiring a new ShapeAnim implementation. Legacy GameCube camera GX-link/light/fog submission is suppressed only on Vita; geometry is captured and replayed by vitaGL.

`mv_onep_mode_run` now owns a real Stage Select loop: mode SSS callback -> `mnStageSel_Scene_OnEnter/OnFrame/OnExit` -> mode SSS exit callback. It records `GAME_SSS_NATIVE_PREPARE_PASS`, `GAME_SSS_CAPTURE_INIT_PASS`, frame telemetry and `GAME_SSS_EXIT`. Back returns cleanly; a confirmed stage advances to the third state instead of returning `-80`.

`MvModeRoute` now describes CSS, SSS and VS/Training state IDs, data buffers and callbacks for VS, Stamina, Super Sudden Death, Giant, Tiny, Invisible, Fixed Camera, Single Button, Lightning, Slow-Mo and Training. Training now uses its own original CSS/SSS/StartMeleeData buffers instead of the generic VS globals. After SSS confirmation the original third-state `on_enter` is called and logs `GAME_MODE_MATCH_PREPARED`.

The third-state integration exposed the real Player/Camera/fighter-data dependencies. Original `camera.c`, `player.c`, `gm_1736.c`, `ftdata.c`, `efasync.c` and the 34 fighter metadata/kind translation units needed by `ftdata.c` are now part of the Vita target. Superseded local Player/progression helpers were removed. With those original sources present, `make -f Makefile.vita` again completes VELF, SELF and VPK packaging. The next active frontier is no longer SSS or fighter metadata: it is entering the retail `gm_Scene_Vs_OnEnter` / `gm_Scene_Training_OnEnter` scene runtime and then integrating the resulting Ground/Fighter/Item/HUD/effect dependencies. No new physical-Vita evidence exists for this source checkpoint yet.


User direction: integrate missing original scene code; no artificial paused
runtime-frontier screen. The v3.22 frontier pause has been removed in source.
Work is in progress; do not install an intermediate package as a finished port.

Current edits: persistent outer scene dispatch, remembered menu parent/selection, current GM state update, heap-generation-safe GObj lifetime, original CSS/SSS callback bindings for VS plus nine Special variants and Training, native MnSlMap conversion, and a real Stage Select loop. The temporary -80 SSS stop is gone. Original gmvs/gm_1884 and the deeper gameplay source set remain in the full-gameplay profile rather than being replaced by a paused on-screen frontier.

Fresh independent ARM object audit: 1045/1182 compile, 137 fail using the audit
script's flags (not the platform build flags). `build/vita/audit/report.json`
contains all errors; this is not a linked gameplay test. It establishes that
full gameplay integration also needs source portability work, not only routing.
No new hardware evidence. Final build/test/hash checks still pending.


## 2026-09-10 — v3.22 / 00.32 integration checkpoint

Full menu graph release is **not complete**. vitaGL remains the renderer.

Implemented in this continuation:
- Scene-owned native `MenuExitData`, separate from Title button storage.
  Reset uses GM_COUNT; Title/CSS resets cannot expose a stale menu target.
- Valid GM exits other than Classic/Adventure/Title stop at
  `GAME_MENU_RUNTIME_FRONTIER`. The original menu shell is retained and paused;
  a fresh Circle press resumes it, Select+Start exits. Frontier has log output
  only, not explanatory on-screen text. Hardware return is not yet verified.
- Node/edge/GM-exit logs, controller port, MENU frame timings, proxy symbol
  offset and owning menu. These are observed transitions, not full coverage.
- Dynamic capture deduplicates by HSD object pointer (512-root bound). Base
  anchors are required on initial entry only; subsequent external panels may
  replace them. Empty capture is an explicit error. Non-JObj objects stay out.
- GmEvent.dat: archive preparation now validates all 51 table entries, evinit
  and first-player pointers, converts both packed evinit flag bytes to the ARM
  bitfield layout, and handles shared init records once per archive load.
  `GAME_MENU_EVENT_NATIVE_PASS` explicitly reports menu-only scope. The old
  unconditional TABLE_PASS has become TABLE_LOADED. Runtime-only scalar tables
  (x4, bonus, stage and player values) still require typed conversion before
  Event gameplay can run; this change does not claim that conversion.

Verification:
- `build/vita/test-env/bin/python vita/tools/test_menu_boundary.py`: PASS on
  compiled ARM code for all 45 GM IDs, separate storage, reset invalidation,
  51 retail Event records, exact flag bytes, preservation of unrelated bytes,
  and rejection of an invalid relocated table pointer.
- Final v3.22 Vita compile/link/package: PASS (exit 0).
  ZIP CRC and bundled eboot match: PASS. VPK contains no ISO or SDK CHM.
  Artifact: `build/vita/SmashMeleeVita-assets.vpk`, 1589615 bytes.
  SHA-256: `dfe192f90d0cfa38ace450a5ef2c7a4f8aba8a1d5740dfce20f7144ca0fc2a65`.
  Build log: `build/vita/menu-graph-routing-build.log`.
  ARM log: `build/vita/menu-boundary-check.log`.
  Evidence: `build/vita/menu-graph-routing-verification.json`.
- Existing linker enum-ABI warnings remain. No new hardware proof in this turn.

Remaining before the single full-graph release:
1. Implement proper GM_TITLE return and CSS Back lifecycle; main still ends
   after these returns. Destruction must precede freeing descriptor assets.
2. Audit enter/exit/re-enter of each external panel and exact parent/selection;
   proxy classes and SIS/SObj text remain incomplete.
3. Add declared reachable-node/edge inventory and coverage counters; current
   per-frame node observation cannot prove external-panel or whole-graph coverage.
4. Add an on-screen runtime-frontier explanation and validate retained-shell
   resume; finish runtime descriptor conversion before promoting any new mode.
5. Build the complete graph, then run the physical navigation sweep described
   below. This checkpoint is not a request for a one-screen hardware release.




## Documentation rule

PORTING_STATUS.md is now a live deliverable. Every future routing, scene, renderer, asset-conversion, linker-frontier, hardware-test or testable-VPK change must update this file in the same iteration before the work is considered ready for handoff/testing.

The detailed full-graph plan is tracked in MENU_GRAPH_STRATEGY.md.

## v3.21 current menu-graph baseline

The strategy has changed from incremental one-screen milestones to a **full graph first, bug-fix second** pass. The target is to connect every reachable retail menu selection before using physical-Vita testing to fix individual routes or visual defects.

Current source state already establishes the broad base for that pass:

- all live internal submenu think callbacks are enabled on Vita, including Regular Match, Stadium, Special Melee and Records;
- the original external menu modules are linked for Event, Multi-Man, Rules, Item/Stage Switch, Name Entry, Rumble, Sound, Display, Language, Data Delete, Snapshot, Gallery, Sound Test, Records/Diagram pages and related Info panels;
- MnMaAll.usd is exposed through a lazy ARM-native archive proxy for named JObj/AnimJoint/MatAnimJoint requests;
- the menu renderer dynamically captures additional live JObjs from the HSD GX-link lists rather than assuming only the MAIN background/panel/content roots exist;
- the broad source set currently links and packages to build/vita/SmashMeleeVita-assets.vpk;
- the executable banner is MELEE_VITA_GAME_BOOT v3.21 and the Vita package version is 00.31;
- Classic is hardware-proven through CSS exit and first-match StartMeleeData preparation. The playable GS_VS fighter/ground/HUD runtime remains a separate gameplay boundary and is not part of the definition of menu-graph completion.

The next implementation pass completes the routing architecture rather than adding another isolated screen: dedicated GM_MENU exit state, persistent parent/selection return semantics, first-class external-panel nodes, ARM-safe conversion of data-only DAT tables such as Event data, a generic terminal GM_* dispatcher, and menu graph node/edge coverage telemetry.

### Full-graph completion criterion

A menu edge is connected when it either enters the correct original submenu/panel or exits with the exact retail GM_* target. If the target gameplay runtime is not ported yet, the dispatcher must stop at an explicit GAME_MENU_RUNTIME_FRONTIER boundary; it must not silently fall back to MAIN/Title or terminate ambiguously.

No hardware bug-fix pass starts until the complete reachable graph has been wired and a single VPK has been produced for a broad navigation sweep.

---

# Porting status — v3.16 Classic/Adventure CSS and first-VS preparation

## v3.16 local result (build verified; hardware pending)

The original live MAIN path can now route 1-P Mode into the original Character Select
state for both Classic and Adventure. The Vita path uses the real `MnSlChr.usd` HSD
objects, original `mnCharSel_Scene_OnEnter/OnFrame/OnExit` state logic, Vita pad input,
and the shared vitaGL GX capture/replay backend. Per-frame telemetry records frame, capture,
replay and present timing while the CSS is active.

After CSS confirmation, v3.16 no longer stops at a synthetic `STAGE_MATCH_PENDING` marker.
It runs the original mode CSS exit callback, the original first preparation/intro state, and
then the original first `GS_VS` state `on_enter`. Classic therefore passes through state 0
before state 1; Adventure passes through `ADVENTURE_INTRO` before
`ADVENTURE_MUSHROOM_KINGDOM`. The resulting `StartMeleeData` is built by the original
1-P rules/roster builder and contains the selected player, CPU roster, stock/rule data and
the first stage kind. Fresh logs end this bounded milestone with
`GAME_1P_PREP_STATE_READY` and `GAME_1P_STAGE_MATCH_READY`.

This is deliberately not yet a claim of playable fighter gameplay. The actual `GS_VS` scene
runtime still depends on fighter, camera, ground/event and HUD islands that are not ported into
the Vita executable. For `MELEE_VITA_PLATFORM` only, v3.16 keeps the original match-data
construction but removes live gameplay callbacks/event side effects that would otherwise drag
those unported islands into the link. Non-Vita state tables and behavior remain unchanged.

The Vita Classic/Adventure state tables are reduced to the states reachable by this milestone
(CSS, preparation/intro and first VS), preventing unreachable later-mode callbacks from forcing
unported gameplay dependencies into the binary. The original full tables remain compiled for
non-Vita targets.

Build validation: `make -f Makefile.vita` completes VELF, SELF and VPK packaging. The known
ARM EABI enum-size linker warnings remain and are still tracked as an ABI cleanup item. Physical
Vita validation of v3.16 is pending; the next hardware test should return the fresh v3.16
`runtime.log` and a CSS screenshot, then confirm the two new post-CSS markers.

Final v3.16 build artifact:

- `build/vita/SmashMeleeVita-v3.16.vpk`: 1,469,689 bytes; SHA-256 `41cac79fd63d036d32290221d003ab0cc08b6f17e6e7bfc4cbcd189fbdd7e55d`.
- `build/vita/eboot.bin`: 1,027,562 bytes; SHA-256 `0c7fbe3f51d07aa1a867e2ee1f9d3c2159bf8c5fd62d1999bfd510ba0b7f56d3`.
- VPK entries: `sce_sys/param.sfo`, `eboot.bin`, `sce_sys/icon0.png`, `sce_sys/pic0.png`, `sce_sys/livearea/contents/bg.png`, `startup.png`, and `template.xml`.
- The legacy `arm-boot-check` is not a v3.16 pass criterion: its Unicorn model currently reaches the existing DevCom/SFX path without modeling the real Vita file load, reports `invalid SFX header size after DevCom load`, and traps in `_kill_r`. The Vita executable itself links/packages successfully; physical-hardware validation remains authoritative for this milestone.

---
# Porting status — v3.13 native MAIN HPS/cardgame frontier
# Porting status — v3.15 live `mnMain` matrix + MatAnim/TexAnim

## v3.14 hardware result / v3.15 second-frame crash + menu material fix

Physical-Vita v3.14 reaches the real original MAIN scene. The verified run
completes the opening movie path, original `GmTtAll.usd` Title, START
transition, GM_MENU preload/cardgame boundary, `MnMaAll.usd` native conversion,
`LbAd.dat`, DevCom SBUF/HPS streaming setup and finally
`mnMain_Scene_OnEnter()`. The first MAIN capture contains 241 GX commands / 2248
triangles and the first vitaGL submit succeeds (`gl_error=0`).

The corresponding core is a branch-to-NULL on the following MAIN tick: the
main thread has `PC=0`, and its runtime LR maps back to
`HSD_JObjSetupMatrixSub()`. Under `MELEE_VITA_HSD_LOAD_ONLY`, the JObj class had
only installed `load`; live `mnMain` animation calls
`HSD_JOBJ_METHOD(jobj)->make_mtx()` on dirty menu nodes, so the method pointer
was NULL. v3.15 installs the original `HSD_JObjMakeMatrix` method while keeping
`make_pmtx`/`disp` on the Vita capture path. This fixes live matrix updates
without re-enabling the GameCube GX renderer.

The v3.14 screenshot also proves that the menu topology is now original but
its material state is not: all five MAIN choices render as `1-P Mode`. The
reason is explicit in the old runtime marker (`matanim=pending`). Original
`mn_8022B3A0()` selects each label with `start_frame + selection * 2` and stops
specific TObj FObjs via `mn_8022F3D8()`, but those operations cannot select a
different TIMG when `StaticModelDesc.matanim_joint` is NULL.

v3.15 converts and attaches the authentic `MnMaAll.usd` MatAnim/TexAnim graphs
for all four live MAIN roots. ShapeAnim inspection reports zero ShapeAnim
objects for these roots, so no ShapeAnim adapter is required for this scene:

- `MenMainBack`: 86 MatAnim / 15 TexAnim;
- `MenMainPanel`: 44 MatAnim / 3 TexAnim;
- `MenMainConTop`: 20 MatAnim / 6 TexAnim;
- `MenMainCursor`: 10 MatAnim / 4 TexAnim.

The release ARM regression builds native AnimJoint + MatAnim for Panel, ConTop
and Cursor, executes `HSD_JObjAnimAll()` plus `HSD_JObjSetupMatrixSub()`, and
captures all three successfully (Panel 47 display lists / 1102 triangles,
ConTop 20 / 177, Cursor 10 / 129). Hardware markers `GAME_MENU_LIVE_PASS` at
frames 1/60/180 distinguish a stable live menu from the old first-frame-only
success.

The lower description (`Solo Smash!` for 1-P Mode) remains intentionally
pending. Upstream `mn_80229A7C()` renders that line through HSD SisLib; the
current Vita MAIN island keeps SIS text disabled until its draw path is routed
to vitaGL. This does not affect the five HSD/TIMG menu labels fixed in v3.15.

Hardware-test artifact built from the real checkout:

- `build/vita/SmashMeleeVita-v3.15-menu-matanim-vitaGL.vpk`
- size: 1,411,630 bytes
- SHA-256: `5ac3f29a1652f4f8ef7a6c0c292249a5756d963cb4c159690a948ce3238016bb`
- SELF SHA-256: `8282f8c0772dce681e24fb45701edd4003004154243a84a911a9a184bc7fae38`

The packaged VPK still contains the user-supplied LiveArea icon/pic/background/
startup/template assets, contains no `.DS_Store`, and the linked map resolves
`vglInitExtended`/`vglSwapBuffers` with no `vita2d_*` symbols.

## v3.13 hardware result / v3.14 live MAIN GObj locator fix

Physical-Vita v3.13 validates the complete stream-audio frontier introduced in
v3.12/v3.13. After START on the original Title, the runtime enters `GM_MENU`,
recreates the menu preload heap, loads the cardgame resources, prepares all four
native `MnMaAll.usd` roots, loads `LbAd.dat`, completes the original DevCom
`0x22` SBUF request and parses both the 32000-Hz stereo HPS header and its first
0x10000-byte stream block. The hardware log reaches:

- `GAME_MENU_NATIVE_PREPARE_PASS`;
- `GAME_MENU_AUDIO_TABLE_PASS`;
- `DEVCOM_SBUF_PASS`;
- `HPS_STREAM_HEADER sample_rate=32000 channels=2 cancel=0`;
- `HPS_BLOCK_HEADER chunk=65536 end=65535 next=000100a0 slot=2`.

The next failure is `GAME_MENU_ORIGINAL_CAPTURE_FAIL code=-1`. This is not a
failed START transition and not a failure inside `mnMain_Scene_OnEnter()`: START
correctly exits the Title and enters the MAIN scene. The failure is in the Vita
capture wrapper's live-GObj lookup. Upstream `GObj_Create()` has signature
`GObj_Create(classifier, p_link, priority)`. The original MAIN objects are
created as `(4,5)`, `(5,6)` and `(6,7)`, with GX links 2/3/4 respectively, but
the wrapper accidentally used the classifier values 4/5/6 as indices into
`HSD_GObj_Entities`. v3.14 fixes the locator to use p_links 5/6/7 and logs
`GAME_MENU_GOBJ_LOOKUP_PASS` with all three live JObj roots before the first
capture. A lookup failure now reports each missing root explicitly.

Expected v3.14 progression is therefore:

1. the same verified v3.13 movie/Title/HPS sequence;
2. `GAME_MENU_GOBJ_LOOKUP_PASS ... plinks=5,6,7 gxlinks=2,3,4`;
3. `GAME_MENU_ORIGINAL_ENTER_PASS commands=... triangles=...`;
4. live `GAME_MENU_SELECTION` markers when UP/DOWN changes the original
   `mn_8022DB10` selection state.

## v3.12 hardware result / v3.13 HPS stream endian + original cardgame setup

Physical-Vita v3.12 validates the DevCom SBUF implementation end-to-end. The
opening movie still decodes and renders through vitaGL, the original Title
advances with native MatAnim/TexAnim, START transitions to `GM_MENU`, native
`MnMaAll.usd` preparation succeeds, and `LbAd.dat` loads successfully. The
stream-audio request then reports `DEVCOM_SBUF_PASS file=9 src=00000000
size=128 type=22`, proving the original `0x22` DVD -> SBUF callback semantics
are now correct on hardware.

The same run exposes two independent lifecycle/ABI issues immediately after
that pass. First, the follow-up `0x21` request targets `lbl_804C4540` at an ARM
address ending in `0x1094`, which is not 32-byte aligned and is rejected by the
GameCube DevCom alignment contract. The subsequent `0x23 size=0` is a
consequence of the failed 0x20-byte block-header read. Second,
`lbCardGame_UpdatePowerTime()` asserts `_p(enable)` because the direct Vita
Title -> MAIN bridge entered `mnMain_Scene_OnEnter()` without the
`gmmenumode.c:onEnter()` cardgame setup normally performed by the game-mode
state machine.

v3.13 fixes both without weakening upstream assertions. `lbl_804C4540[3]` is
explicitly 32-byte aligned on Vita. HPS headers are converted from their native
GameCube byte order before the original stream code consumes them: the initial
0x80-byte header now reads the 32000-Hz sample rate/channel count as BE32 and
converts `AXPBADDR`/`AXPBADPCM` BE16 fields; each 0x20-byte stream block swaps
its `x0/x4/x8` fields plus loop-state halfwords before scheduling the normal
`0x23` ARAM transfer. Real extracted HPS files verify the expected first block
values `x0=0x10000`, `x4=0xffff`, `x8=0x100a0` for the common menu stream
layout. Runtime markers are `HPS_STREAM_HEADER` and `HPS_BLOCK_HEADER`.

Before entering `mnMain_Scene_OnEnter()`, the Vita path now also performs the
same cardgame boundary used by `gmmenumode.c:onEnter()`:
`lbCardNew_AllocWorkArea()` followed by `lbCardGame_LoadArchive(0)`. This loads
the original `LbMcGame`/`NtMemAc` resources and sets the cardgame subsystem
enabled before MAIN calls `lbCardGame_UpdatePowerTime()`. The new marker is
`GAME_MENU_CARDGAME_PASS`. No internal enable flag is forged by the Vita
wrapper.

Expected v3.13 hardware progression after START is therefore:

1. `GAME_MENU_NATIVE_PREPARE_PASS`;
2. `GAME_MENU_AUDIO_TABLE_PASS`;
3. `DEVCOM_SBUF_PASS`;
4. `HPS_STREAM_HEADER sample_rate=32000 channels=2 cancel=0`;
5. an aligned `0x21` block-header read followed by
   `HPS_BLOCK_HEADER chunk=65536 ...` and the normal `0x23` stream transfer;
6. `GAME_MENU_CARDGAME_PASS` and then `GAME_MENU_ORIGINAL_ENTER_PASS`.

Title visual fidelity is still explicitly unfinished. v3.11/v3.12 prove the
native animation graph and vitaGL path, but remaining custom TEV/multitexture
material semantics still separate the Vita Title from the GameCube reference.

## v3.11 hardware result / v3.12 DevCom `0x22` SBUF support

Physical-Vita v3.11 validates the new Title MatAnim/TexAnim path and advances
past the previous MAIN preload failure. The original opening movie still decodes
and renders through vitaGL, `GmTtAll.usd` reaches the live Title loop, and START
transitions into the original `mnMain_Scene_OnEnter()` path. MAIN preload,
`mv_menu_vita_prepare()` and the `LbAd.dat` / `lbAudioLoadData` archive load all
complete successfully on hardware.

The next failure is no longer heap, archive, file I/O or renderer related. The
hardware log ends on `DEVCOM_FAIL reason=type ... type=22` from the original
stream-audio startup. Upstream HSD defines type `0x22` as DVD -> DevCom SBUF:
`dest == 0` is intentional, up to `DEVCOM_BUF_SIZE` bytes are read into an
internal 32-byte-aligned relay buffer, and that buffer pointer is supplied to
the completion callback. `HSD_Synth_8038B5AC()` uses exactly this path to read
the first 0x80-byte stream header before scheduling the normal `0x21` / `0x23`
follow-up transfers.

v3.12 implements that exact synchronous Vita equivalent. The adapter now accepts
`0x22`, validates source/size alignment without requiring a destination pointer,
enforces the 0x4000-byte SBUF limit, reads into the existing aligned DevCom relay
buffer, and invokes the original callback with that buffer. Success is logged as
`DEVCOM_SBUF_PASS`; invalid callback, oversized SBUF or DVD read failures remain
fatal and retain explicit diagnostic markers. No renderer fallback or vita2d
path is introduced.

The v3.11 screenshot also confirms a substantial Title fidelity improvement:
`Melee`, `TM`, `PRESS START` and copyright layers are now present and animated
from the original MatAnim/TexAnim data. It is still not visually GameCube-faithful
yet: remaining differences are concentrated in custom TEV / multitexture
compositing and some animated material transforms, so Title fidelity remains an
active renderer task after the MAIN boot frontier.

## v3.10 hardware result / v3.11 native Title MatAnim + exact two-texture vitaGL

Physical-Vita v3.10 reaches the original `GmTtAll.usd` Title and accepts START,
but the visible result is not yet faithful to GameCube: the logo/copyright are
present while the background is dominated by a green wireframe-like layer and
the original `Melee` / `PRESS START` material effects are incomplete. The same
run identifies the following MAIN failure after START: `lbHeap_80015BD0()`
returns NULL before the `LbAd.dat` DevCom request because the direct Vita scene
bridge skipped the original preload heap lifecycle. v3.11 restores the
`lbDvdPreload_2 -> lbHeap_80015900()` boundary before `mnMain_Scene_OnEnter()`
and reinitializes SisLib, rather than bypassing the original archive load.

The Title fidelity work is now data-driven from the verified original
`GmTtAll.usd`. ARM capture reproduces the hardware scene exactly: 77 GX commands
and 2681 triangles. `TtlBg` contributes 37 commands, 36 of which are two-texture
materials. 34 of those 36 use the same exact two-layer HSD MODULATE graph and
are now executed directly by vitaGL with independent animated UV matrices;
only two background commands still require broader custom-TEV support.

The missing Title animation is not ShapeAnim: both `TtlBg` and `TtlMoji` have
zero ShapeAnim objects. v3.11 adds a typed big-endian -> ARM converter for
`HSD_MatAnimJoint`, `HSD_MatAnim`, `HSD_TexAnim`, `HSD_AObjDesc`,
`HSD_FObjDesc`, animated image tables and TLUT tables. Those converted graphs
are passed to the original `HSD_JObjAddAnimAll()` path. The verified counts are:

- `TtlBg`: 6 MatAnimJoint nodes, 5 MatAnim, 2 TexAnim, 6 AObj, 14 FObj;
- `TtlMoji`: 31 MatAnimJoint nodes, 29 MatAnim, 21 TexAnim, 44 AObj, 113 FObj;
- `TtlMoji` references 452 animated ImageDesc entries; 450 are 32x32 and two
  are 176x40, totaling only about 1.81 MiB when decoded to RGBA.

The ARM runtime test attaches both native MatAnim graphs to live HSD JObjs,
samples the GameCube title frames (background 130, logo 400), captures the same
77-command stream, then advances each graph for 600 additional HSD animation
ticks without an assertion, invalid TIMG index or trap. The original lightweight
Title animation procs are enabled on Vita again so MatAnim/TexAnim continue to
advance every rendered frame.

vitaGL texture caching is also changed for live material animation. UV matrices
and PE state no longer create duplicate decoded textures, common HSD
MODULATE/REPLACE material color is applied dynamically with the GL vertex color,
and the cache is heap-backed with 512 entries plus LRU eviction. This avoids
both the old 128-entry stack footprint and permanent draw failure when animated
TIMG sequences exceed the historical cache. Hardware logs report cache usage at
frames 0/120/300 through `GAME_TITLE_TEXTURE_CACHE`.

The Vita package now also embeds the supplied LiveArea assets from `vita/sce_sys`:
128x128 `icon0.png`, 960x544 `pic0.png`, 840x500 LiveArea background,
280x158 startup image and `template.xml`. Finder metadata such as `.DS_Store`
is deliberately excluded from the VPK.

Expected v3.11 hardware markers:

1. `GAME_TITLE_MATANIM_NATIVE_PASS moji_joints=31 ... moji_fobj=113 bg_joints=6 ... bg_fobj=14`;
2. `VITAGL_REPLAY_READY ... capacity=512 ...`;
3. `GAME_TITLE_LIVE_ANIM frame=120 ...` and `GAME_TITLE_TEXTURE_CACHE frame=120 ...`;
4. visually, substantially more of the original red/blue radial background and
   animated Title material layers should be present. The remaining visual gap is
   expected to concentrate in the 15 `TtlMoji` multitexture commands and custom
   TEV patterns not yet translated exactly.

## v3.9 hardware result / v3.10 `LbAd.dat` DevCom diagnostics + DVD fallback

Physical-Vita v3.9 validates the native MAIN asset conversion added after the
v3.8 `MenMainPanel` failure. The runtime reaches
`GAME_MENU_NATIVE_PREPARE_PASS` for `Back+Panel+ConTop+Cursor`, so the v3.9
billboard/envelope/PNMTXIDX descriptor path completes on real hardware before
the next failure.

The new failure is an intentional `HSD_Panic`, not a renderer Data Abort:
`lbFile_8001615C()` asserts `!cancelflag` at `lbfile.c:18`. The next operation
after `mv_menu_vita_prepare()` in the original `mnMain_Scene_OnEnter()` is
`lbAudioAx_8002392C()`, which loads `LbAd.dat` / `lbAudioLoadData` through
`lbArchive_LoadSymbols()`. The verified local disc file is 14488 bytes
(14496 bytes after the GameCube 32-byte DVD round-up).

v3.10 keeps the upstream assertion intact and makes the Vita DVD/DevCom bridge
diagnosable instead of hiding a failed request. `HSD_DevComRequest()` now logs
distinct `DEVCOM_FAIL` reasons for unsupported type, zero request, alignment,
`DVDFastOpen`, CPU DVD read and ARAM DVD read. `DVDReadPrio()` still prefers
`sceIo*`, but if native file I/O fails it retries with newlib stdio; that path
is already exercised on hardware by the MTH/title/menu asset loaders. A
successful fallback is reported as `DVD_READ_FALLBACK_PASS`; failure of both
backends stays fatal and reaches the original assertion.

`mnMain_Scene_OnEnter()` brackets the audio-table load with
`GAME_MENU_AUDIO_TABLE_BEGIN` / `GAME_MENU_AUDIO_TABLE_PASS`. The next hardware
run therefore proves either that v3.10 advances beyond `LbAd.dat`, or identifies
the exact DevCom cancellation reason in the same run.

The release ARM regression after these changes remains green on the verified
`MnMaAll.usd`: `MenMainPanel` captures 47 display lists / 1102 triangles,
`MenMainConTop` 20 / 177, and `MenMainCursor` 10 / 129, all with zero capture
errors. No renderer fallback was reintroduced; the game target remains
vitaGL-only.

## v3.8 hardware result / v3.9 native MAIN envelope + billboard capture

Hardware v3.8 is the first run that proves the complete visible game-first chain reaches the real
MAIN menu island with vitaGL.  The physical-Vita runtime log proves:

- the global vitaGL context initializes successfully;
- original HSD/audio/SFX initialization completes through `GMMAIN_FINAL_INIT_PASS`;
- original `MvOpen.mth` is parsed as MTHP, THP-JPEG reconstruction/TurboJPEG decode succeeds,
  frame 0 is submitted and presented through vitaGL, and the movie can be skipped by user input;
- the original `GmTtAll.usd` Title scene is created, captured and animated live;
- START is read by the Title and causes `GM_TITLE -> GM_MENU`;
- execution enters the original `mnMain_Scene_OnEnter()` from `src/melee/mn/mnmain.c`.

The v3.8 crash is therefore no longer a boot, movie, Title or vitaGL-context problem.  It occurs at
the explicit Vita assertion around `mv_menu_vita_prepare()` because native conversion stopped on
`MenMainPanel_Top_joint` with result `1`.

Host inspection of the verified `MnMaAll.usd` identifies the exact missing HSD features instead of
treating the failure as a generic parser error:

- `MenMainBack_Top_joint`: already complete;
- `MenMainPanel_Top_joint`: uses `JOBJ_BILLBOARD` and twelve `POBJ_ENVELOPE` PObjs;
- `MenMainConTop_Top_joint`: uses `POBJ_ENVELOPE`;
- `MenMainCursor_Top_joint`: uses `POBJ_ENVELOPE`.

v3.9 extends the typed native descriptor converter and the GX-capture-to-vitaGL path specifically
for those original Melee features:

- `JOBJ_BILLBOARD` is accepted by the descriptor converter while other unported billboard modes,
  quaternion/IK/user-matrix/RObj paths remain fail-closed;
- `HSD_EnvelopeDesc**` arrays are converted to native little-endian descriptors with deferred joint
  fixups after the complete JObj tree exists;
- the load-only HSD runtime loads/resolves `POBJ_ENVELOPE` instead of panicking;
- the capture precomputes live JObj model matrices because the load-only class intentionally does
  not install the original GX rendering callbacks;
- `HSD_PObjCaptureVita()` reproduces the envelope palette construction performed by
  `SetupEnvelopeModelMtx()`, including the real 0.5/0.5 two-joint blends present in
  `MenMainPanel_Top_joint`;
- GX `PNMTXIDX` is preserved per vertex.  The command-list decoder applies the selected palette
  position matrix to each vertex before vitaGL replay, then resets the command model transform to
  identity to avoid double transformation.

The native converter now reports `unsupported=0` for all four MAIN roots.  Release-ELF ARM tests
exercise the same runtime chain used by the game (`HSD_JObjLoadJoint -> mv_hsd_gx_capture_runtime`)
and pass with the verified `MnMaAll.usd`:

- Back: 86 display lists, 96 commands, 324 triangles;
- Panel: 47 display lists, 1102 triangles;
- ConTop: 20 display lists, 177 triangles;
- Cursor: 10 display lists, 129 triangles.

The full host asset audit remains green: 861 standard HSD archives pass, 0 fail, 33 files are
reported as expected nonstandard containers, and 357 menu textures decode successfully.

v3.9 still does **not** claim final visual fidelity.  Camera-facing billboard orientation is not yet
applied as an exact render-space transform by the capture layer, and the MAIN MatAnim/ShapeAnim
coverage remains incomplete.  Those are visual/runtime frontiers to validate after physical-Vita
hardware proves the original menu can now be constructed and presented without the v3.8 assertion.

Expected v3.9 hardware frontier after START on Title:

1. `GAME_MENU_ORIGINAL_ON_ENTER_BEGIN ... source=mnmain.c ... renderer=vitaGL`;
2. `GAME_MENU_NATIVE_PREPARE_PASS ... billboard=JOBJ_native envelope=HSD_palette_capture pnmtxidx=per_vertex vita_renderer=vitaGL ...`;
3. `GAME_MENU_ORIGINAL_ENTER_PASS commands=... triangles=... selection=...`;
4. UP/DOWN should emit `GAME_MENU_SELECTION ... source=mn_8022DB10`.

### v3.9 artifact

- `build/vita/SmashMeleeVita-v3.9-native-menu-envelope-vitaGL.vpk`: 955890 bytes;
  SHA-256 `7e8651e14899591edf273f6c06338b3fc9aba81f6280521f3fed7d92b4087ebd`.
- `build/vita/eboot.bin`: 961017 bytes;
  SHA-256 `6414b0ce469e0c9698bf5062f99340628c6aca57ed184bb14b248e38b31df899`.
- `build/vita/melee_vita`: 4656096 bytes;
  SHA-256 `713fe509365f61d16f83697e721e76fe175fb7fd949373acd2e5df1ffe7ec13e`.

This artifact is build/host/ARM validated; physical-Vita v3.9 validation is pending.

## v3.7 hardware result / v3.8 opening-movie Data Abort fix

Hardware v3.7 proves the vitaGL context is now initialized correctly: the log reaches
`VITAGL_INIT_PASS`, completes HSD/audio/SFX initialization through `GMMAIN_FINAL_INIT_PASS`, opens
the original `MvOpen.mth`, validates its MTHP header and reaches `GAME_OPENING_BEGIN`.

The first movie draw then raises a main-thread Data Abort.  The v3.7 core dump reports runtime PC
`0x81143042` and LR `0x8106F8ED`; after applying the module relocation, the LR lands on the first
`glVertex2f()` in `mv_opening_movie_run()`.  Disassembly confirms the fault occurs as the first
immediate-mode vertex is submitted, after TurboJPEG has already decoded the frame.

Inspection of the vitaGL implementation identifies the deterministic cause: the v3.6/v3.7 renderer
passed `pool_size=0` to `vglInitExtended()`.  That first argument is vitaGL's GL1 immediate-mode
vertex pool.  `scene_reset()` only allocates `legacy_pool_ptr` when `legacy_pool_size` is non-zero,
while `glVertex3f()` writes vertex data directly through `legacy_pool_ptr`.  The MTHP movie player
and the GX replay both use `glBegin/glVertex*`, so the zero pool leaves the first vertex write aimed
at NULL.

v3.8 restores a 4 MiB legacy pool while retaining the correct v3.7 interpretation of
`vglInitExtended()`'s resolution-fallback return value.  First-frame file-only markers were added:

- `GAME_OPENING_FRAME0_DECODE_PASS` after THP-JPEG reconstruction/TurboJPEG RGBA decode;
- `GAME_OPENING_FRAME0_DRAW_BEGIN` immediately before the first vitaGL quad;
- `GAME_OPENING_FRAME0_DRAW_PASS` after the first complete draw/present.

Expected v3.8 hardware frontier is a visible first frame from `MvOpen.mth` followed by
`GAME_OPENING_FRAME frame=1/3036 ...`. START/CROSS should skip the movie and continue to Title.

### v3.8 artifact

- `build/vita/SmashMeleeVita-v3.8-vitaGL-legacy-pool.vpk`: 953146 bytes;
  SHA-256 `5ab8872765ded3910034840a7fc31b261f676dcdea7a775e610b30f60f4e3d5e`.
- `build/vita/eboot.bin`: 958497 bytes;
  SHA-256 `d3d1c98993582d22404f149f5775f66115e2380d14a5ebf33106a0d3f6b50998`.
- `build/vita/melee_vita`: 4650536 bytes;
  SHA-256 `c107729770b6bc12a094b5c4686c2cac82db7188969e699f5ff4545d9471a73f`.

## v3.6 hardware result / v3.7 vitaGL init fix

Hardware v3.6 showed only the vitaGL splash and the runtime log stopped at:

- `VITAGL_INIT_BEGIN ... shader_stat=00000000`;
- `VITAGL_INIT_FAIL code=-1 shader_stat=00000000`.

`shader_stat=0` proves `ur0:data/libshacccg.suprx` is present.  Inspection of the installed/current
vitaGL implementation identified the actual bug in the port wrapper: `vglInitExtended()` returns a
resolution-fallback flag, not a generic success flag.  On a normal 960x544 Vita display it completes
initialization, sets vitaGL's internal `vgl_inited` state, shows the splash, and returns `GL_FALSE`;
`GL_TRUE` only means the requested framebuffer size was clamped to the maximum display size.  The
v3.6 wrapper incorrectly treated normal `GL_FALSE` as failure and exited immediately after the
splash.

v3.7 removes that incorrect return-value test.  `mv_render_init()` now treats the value returned by
`vglInitExtended()` only as resolution-fallback information and validates the live GL context with
`glGetString(GL_VERSION)` / `glGetString(GL_RENDERER)`.  The renderer remains initialized once,
before HSD/audio allocations, and remains shared across `MvOpen.mth -> Title -> MAIN`.

The v3.7 ARM/Vita build succeeds through VELF/SELF/VPK packaging in the real source checkout.
Hardware validation is required next; the expected decisive marker is `VITAGL_INIT_PASS`, followed
by normal HSD/audio boot and `GAME_OPENING_BEGIN` or the Title fallback path.

### v3.7 artifact

- `build/vita/SmashMeleeVita-v3.7-vitaGL-returnfix.vpk`: 952848 bytes;
  SHA-256 `0c1116a150af8a6bade46140ecfc3ae1603d47c0091946aa28aa9ab633f928ba`.
- `build/vita/eboot.bin`: 958338 bytes;
  SHA-256 `879a73bcbf833c9ae907aab863031a3afdc579ec753d89705a750c624951e3b5`.
- `build/vita/melee_vita`: 4650440 bytes;
  SHA-256 `390579912f4084fd6ee38124038b0809150fc7e745349db8e928ba1d23edd4f8`.

## v3.5 original MAIN menu island on vitaGL (build OK; hardware pending)

The static `menu_boot_vita.c` composition bridge has been replaced for `MENU_KIND_MAIN`.
The Vita path now converts the real `MnMaAll.usd` roots to native little-endian HSD descriptors,
attaches their real AnimJoint graphs, then calls `mnMain_Scene_OnEnter()` from the original
`src/melee/mn/mnmain.c`.  The resulting live GObjs/JObjs are advanced by the original HSD proc
scheduler and captured every frame for the vitaGL replay backend.

The linked MAIN island now contains the original functions `mnMain_Scene_OnEnter`,
`mn_8022DB10`, `fn_8022AFEC`, `mn_80229B2C`, `mn_80229DC0`, `mn_8022B3A0`, plus the original
JObj traversal helpers `lb_80011E24` and `lb_8001204C` from `lbspdisplay.c`.  This is no longer a
one-time capture of four static roots: Melee itself creates the background, panel, five option
branches and cursor JObjs and updates hover animation/state.  The Vita loop calls
`HSD_PadRenewStatus()`, `gm_EvaluateAllControllerInputs()` and `HSD_GObj_80390CFC()` before each
live capture, so UP/DOWN navigation is driven by the original `mn_8022DB10` input logic.

The 50-symbol v3.4 `mnMain` frontier is closed for this bounded MAIN island.  The build achieves
that without success stubs: compile-time Vita pruning removes references to unported submenu think
functions; fog/light GameCube GX callbacks and SIS description text are deliberately excluded from
this first island because the active renderer is vitaGL.  Confirmation/back currently play the
original UI SFX but remain on MAIN instead of entering an unported submenu or writing through the
temporary Title exit-data shim.  1P/VS/Trophies/Options/Data transitions are therefore still
explicitly pending, not claimed working.

Native `MnMaAll` data prepared for the original code in v3.5:

- `MenMainBack_Top_joint` + `MenMainBack_Top_animjoint`;
- `MenMainPanel_Top_joint` + `MenMainPanel_Top_animjoint`;
- `MenMainConTop_Top_joint` + `MenMainConTop_Top_animjoint`;
- `MenMainCursor_Top_joint` + `MenMainCursor_Top_animjoint`;
- `ScMenMain_cam_int1_camera` converted to a native HSD perspective-camera descriptor.

MatAnim/ShapeAnim, SIS description text, fog and HSD light objects are still not connected to the
MAIN island.  The current vitaGL replay still has incomplete general GX TEV coverage, so a correct
link/build does not prove final visual fidelity.  The next physical-Vita run must confirm the
original panel/options/cursors are visible and that UP/DOWN changes the highlighted selection.

v3.5 build validation:

- `make -f Makefile.vita` succeeds through ARM link, VELF, SELF and VPK packaging in both the
  managed worktree and the real source checkout;
- final map contains `mnMain_Scene_OnEnter`, `mn_8022DB10`, `fn_8022AFEC`, `lb_80011E24`,
  `lb_8001204C`, `vglInitExtended` and `vglSwapBuffers`;
- final map contains no `vita2d_*` symbol;
- boot marker is `MELEE_VITA_GAME_BOOT v3.5`; VPK version is `00.23`;
- host `asset-check` passes: 861 HSD archives passed / 0 failed, 357 menu textures decoded,
  and `MenMainBack` frame-0 AnimJoint reports 102 joints / 87 FObjs / 0 unsupported;
- the legacy `arm-check` reaches Rand, PAD, HSD geometry and AnimJoint PASS, then stops because
  its old harness expects the test-only symbol `mv_matanim_frame0_stats` in the release ELF.
  `--gc-sections` now removes that unreferenced helper; it is not forced back into the VPK merely
  to satisfy the old test harness;
- hardware validation of v3.5 is pending.

### v3.5 artifact

- `build/vita/SmashMeleeVita-v3.5-native-main-vitaGL.vpk`: 951982 bytes;
  SHA-256 `b3110df7045366b7fd19da469e4ca02546208bef79b181697f606b18ed655e51`.
- `build/vita/eboot.bin`: 957839 bytes;
  SHA-256 `717f3a807b54d30c8e620bb64c4cbfb9f4bd1fd3cc73e14d7d1dfd39be6f1a45`.
- `build/vita/melee_vita`: 4650180 bytes;
  SHA-256 `c0f7fd530933ab7681f63390817ff7121be90f3ff78e39e66b97095cd654719a`.

Expected v3.5 log frontier after START on Title:

1. `GAME_MENU_ORIGINAL_ON_ENTER_BEGIN ... source=mnmain.c ... renderer=vitaGL`;
2. `GAME_MENU_NATIVE_PREPARE_PASS ... roots=Back+Panel+ConTop+Cursor`;
3. `GAME_MENU_ORIGINAL_ENTER_PASS commands=... triangles=... selection=...`;
4. moving UP/DOWN should emit `GAME_MENU_SELECTION ... source=mn_8022DB10`.

## v3.4 renderer and SDK-reference policy

vitaGL is now mandatory for the game target. CMake no longer exposes a vita2d comparison
backend: `melee_vita` always compiles `gx_replay_vitagl.c` + `render_vitagl.c`, defines
`MELEE_VITAGL=1`, and links vitaGL/vitashark/Shacc. The final link map contains
`vglInitExtended` and `vglSwapBuffers` and contains no `vita2d_*` symbol. The old vita2d
sources remain in the tree only as historical/reference code and are not part of the VPK.

The user-supplied `PS_Vita_SDKDoc.chm` is stored locally under ignored `local_docs/` and was
fully extracted to `local_docs/psvita-sdk/`: 18,624 CHM entries were enumerated, the HHC TOC
contains 1,361 nodes, the HHK index 28,366 roots, and extraction reported no warnings.
`local_docs/` is in `.gitignore`; neither the CHM nor the extracted Sony documentation is to be
committed, pushed, or packaged in the VPK.

The following Sony SDK constraints are now explicit renderer requirements for the port:

- GXM state changes and draws must occur inside a BeginScene/EndScene pair; vitaGL owns that
  scene lifecycle for the port rather than game code reaching into a vita2d/GXM context.
- textures, surfaces and vertex/index data consumed by the GPU require correctly mapped GPU
  memory; shader code requires the corresponding USSE mappings. vitaGL's allocators/runtime
  remain the owning abstraction unless a measured porting requirement needs direct GXM memory.
- display buffers require the documented alignment/stride rules and display/GPU synchronization;
  frame pacing must use the display/vblank path rather than CPU polling.
- texture filtering/addressing is translated through GL texture state backed by the documented
  GXM texture controls. GX repeat/clamp/mirror semantics must be preserved instead of silently
  accepting unsupported sampler state.
- GXM blending is part of fragment-program patching. vitaGL blend state is therefore treated as
  shader/render-state semantics, not as a post-process approximation; unsupported GX TEV/blend
  graphs remain explicit implementation work.

The v3.4 vitaGL-only ARM/Vita build links and packages successfully. Hardware validation is still
required before any new visual, timing or memory claim is marked proven.

### v3.4 original `mnMain` link frontier

The next runtime target was also audited without enabling it on hardware. `mnmain.c` itself now
compiles for ARM32 and can be pulled into the Vita link. Forcing references to
`mnMain_Scene_OnEnter()` / `mnMain_Scene_OnFrame()` exposes 50 unique unresolved symbols rather
than an unbounded dependency graph. The inventory is saved locally under
`build/vita/mnmain-link-frontier-v3.4.txt`.

The frontier groups are actionable:

- JObj traversal helpers: `lb_80011E24`, `lb_8001204C`, plus the light-list helper
  `lb_80011AC4`.
- main-menu input/state glue: `gm_801A36C0` and a small set of gm state helpers.
- fog/light and EFB erase paths that still call fixed-function GX. These must be translated or
  bypassed at the HSD-to-vitaGL boundary; they must not be implemented by reintroducing vita2d.
- SIS text functions used by menu descriptions/names.
- submenu entrypoints referenced by the generic menu table (Event, Sound, Data, Gallery, etc.).
  They do not need to be enabled to bring up the main menu first; the Vita path should prune or
  defer those transitions until each submenu dependency island is ported.

This audit confirms the preferred direction: prepare native `MnMaAll` JObj/AnimJoint data like the
Title path does, run the original main-menu GObj/update/input logic, and render its live HSD objects
through the vitaGL replay backend. The current static menu bridge remains only until that native
main-menu island is executable.

### v3.4 artifact

- `build/vita/SmashMeleeVita-v3.4-vitaGL-only.vpk`: 937026 bytes;
  SHA-256 `25a2c8739ea6a073a4c4b2a6f1d71daf9771cfc17346b2ec44fb75a61e0260b8`.
- `build/vita/eboot.bin`: 942675 bytes;
  SHA-256 `7b3b65072f0836d362bcd40c38f798764f089a8590504f2ee7bdd777629a1b94`.
- `build/vita/melee_vita`: 4616840 bytes;
  SHA-256 `a9461c7efd11f701b3f9f9af5c66a6e7f693c8fd6719d57eb6e227145c7ea1a0`.

This artifact is build-verified only; physical-Vita verification is pending.

## Current facts; supersedes earlier v3.2 claims

User explicitly requested vitaGL and supplied PS_Vita_SDKDoc.chm. Before this change the
source/build used vita2d/GXM, not vitaGL. The previous v3.2 report overstated implementation:
THP unpacking was absent from mth_player_vita.c, title model animation pointers were NULL,
and menu_boot_vita.c captured once then repeatedly drew the same commands. The generic menu
scene OnEnter/OnFrame is still not connected. Original assets and scene-like log labels alone
do not establish a complete native game boot.

The v3.3 transition introduced gx_replay_vitagl.c/render_vitagl.c and a shared vitaGL context
for movie, title and menu. v3.4 closes that migration by removing the selectable vita2d backend
from the game target entirely. Visuals, memory use and timing still require physical-Vita proof.

The supplied CHM is extracted under ignored `local_docs/psvita-sdk/`; relevant libgxm,
Display, Memory Management and Graphics Programming references are used as implementation
constraints. Movie texture reuse and resource destruction currently use explicit completion waits
via glFinish on vitaGL. No supplied SDK documentation is part of the repository payload or VPK.

## Intro correction

thp_jpeg.c converts Nintendo unstuffed entropy bytes to a baseline JPEG stream before TurboJPEG.
It parses bounded marker segments, requires a final EOI, handles frame padding, and checks output
capacity before writing. The previous reader would reject frame 2650 because its padded block is
61184 bytes while the header maximum is 61152; the bounded buffer now allows the extra 32 bytes.
The exact shared C conversion plus host TurboJPEG decoded all 3036 frames (640x480, 30 fps).
Report: build/vita/host/movie-check.json; log: build/vita/movie-check-v3.3.log.
This validates CPU decoding, not Vita playback cadence, sound or visual presentation.

## Title runtime correction (validation in progress)

hsd_anim_native.c converts bounded AnimJoint/AObj/FObj descriptors while preserving compressed
FObj byte streams. The title now attaches real animation graphs and registers original gmtitle
animation processes (mn_8022ED6C). Capture reads persistent live JObjs each frame, respects
JOBJ_HIDDEN and does not reconstruct a separate static runtime graph each frame. Texture cache
preparation sees all title geometry before the visibility-filtered captures begin.
MatAnim, fog/lighting and the full original title/menu state manager remain incomplete.
A dedicated ARM test checks actual AObj time/poses and stable HSD heap usage; results follow.

## vitaGL renderer scope

GL now performs projection/clipping, perspective interpolation, culling, depth and alpha/additive
blending. Original GX tiled textures are decoded on CPU. Known single-stage material/custom-TEV
bakes are retained. The recognized two-texture background graph uses two GL texture units with
RGB multiplication and previous-alpha preservation, instead of the former GXM offscreen target.
The prior relaxed title/material compatibility path still exists and is not general TEV support.
Menu interaction/animation requires original mnMain integration, not a renderer switch alone.
Current runtime.log in build/vita still belongs to WiiCompiled and is not current Melee evidence.

## Previous reports (historical; use current facts above)

# Porting status — 2026-09-09, v3.2 intro/title/menu integration (build OK; hardware pending)

## Current development assessment

The current boot path is no longer the v2.6 bounded GS_MEMCARD probe. A fresh physical-Vita v3.1
run reaches the original Melee initialization through `gmMainLib_8015FBA4`, loads the real SFX
banks, attempts the original opening movie, renders the original Title assets, accepts START and
transitions to GM_MENU. Diagnostics are file-only; the visible path is Melee. No commit or push is
part of this work.

The remaining frontier is visual/runtime fidelity rather than simply reaching the title. v3.1
proves the title scene and START transition, but the opening movie decoder failed on THP-JPEG
packing and GM_MENU rendered only the `MenMainBack` background layer. v3.2 fixes both of those
frontiers locally and builds successfully, but v3.2 has not yet been validated on physical Vita.

## v3.1 physical-Vita result

Latest hardware log markers:

- `MELEE_VITA_GAME_BOOT v3.1`.
- `GMMAIN_FINAL_INIT_PASS source=gmmain.c mainlib_reset=1 next=GM_TITLE_original_scene`.
- Five real SFX headers load successfully, including `main.ssm` with
  `header=00004b70 sample=001f3780 count=246 base=0 cancel=0`.
- Opening movie header is accepted as MTHP v2, 640x480, 30 fps, 3036 frames.
- The v3.1 player then fails at frame 0 with TurboJPEG
  `Unsupported marker type 0x3f` and falls back to Title.
- Title enters through `gmtitle.c`, captures `TtlBg + TtlMoji`, and the Vita screenshot shows the
  original 4:3 Smash Bros. title geometry rather than a diagnostic app. The visible title is still
  incomplete: `Melee`, `START` and copyright elements are missing.
- START is accepted at frame 1990 and the game transitions `GM_TITLE -> GM_MENU`.
- GM_MENU proves the root cause of the incorrect second screenshot: only
  `MenMainBack_Top_joint` builds successfully. `MenMainPanel_Top_joint`,
  `MenMainConTop_Top_joint` and `MenMainCursor_Top_joint` are skipped by the strict native
  converter, so v3.1 displays only the abstract animated menu background, not the real menu UI.

This is the first hardware run in this project that visibly reaches the original Title and accepts
START into GM_MENU. It is not yet a claim of a complete or interactive main menu.

## v3.2 local implementation

### Opening movie

`MvOpen.mth` itself is valid. Frame 0 begins with a real JPEG `FFD8`; Nintendo THP entropy data does
not use standard JPEG `FF 00` byte stuffing, which is why TurboJPEG treated entropy bytes such as
`FF 3F` as invalid markers. The Vita player now converts each THP-JPEG payload into a standard JPEG
stream before TurboJPEG decode, preserving real JPEG markers and stuffing entropy `FF` bytes.

Independent host validation on the real `MvOpen.mth` rebuilt frame 0 to a 7901-byte standard JPEG;
`djpeg` accepts it and decodes a 640x480 frame. This closes the exact v3.1 frame-0 decoder failure.
The complete 3036-frame movie still requires physical-Vita validation before the intro is marked
hardware-proven.

### Title animation visibility

The typed HSD animation sampler now supports JObj animation channels 11/12 (`NODE`/`BRANCH`) in
addition to R/T/S. These channels drive visibility of title subtrees and were the missing feature
behind the incomplete v3.1 title. On the real `GmTtAll.usd`:

- `TtlMoji_Top_joint` at original frame 400: 31 joints, 41 applied channels, 0 unsupported.
- `TtlBg_Top_joint` at original frame 130: 6 joints, 7 applied channels, 0 unsupported.

v3.2 applies those sampled R/T/S + NODE/BRANCH values directly to the native HSD descriptor graph
before GX capture. This is intended to restore the original `Melee`, `START`, copyright and other
frame-controlled title branches. Hardware validation is still pending.

### Main menu composition

A host probe on the real `MnMaAll.usd` confirms the three UI roots rejected by the strict native
converter are valid and materializable:

- `MenMainPanel_Top_joint`: 106 joints, 262 triangles.
- `MenMainConTop_Top_joint`: 42 joints, 14 triangles.
- `MenMainCursor_Top_joint`: 14 joints, 127 triangles.

Their AnimJoint graphs also sample at frame 0 with 0 unsupported animation channels after the
NODE/BRANCH fix. v3.2 keeps the hardware-proven GX replay for `MenMainBack`, then draws the original
Panel/ConTop/Cursor geometry and textures above it in the same original 4:3 camera. This removes the
v3.1 background-only failure.

This is still an intermediate bridge, not yet full execution of `mnMain_Scene_OnEnter()` /
`mnMain_Scene_OnFrame()`. The original menu code additionally instantiates per-option cursor objects,
applies sub-joint hover frames, updates `hovered_selection`, handles UP/DOWN/CROSS/B and performs
scene transitions. Full main-menu interactivity remains the next runtime milestone. Advanced MatAnim
coverage is also incomplete and must not be described as fully faithful yet.

### v3.2 build validation

- `make -f Makefile.vita`: exit 0 through VELF, SELF and VPK packaging.
- Current worktree VPK: `build/vita/SmashMeleeVita-assets.vpk`, 611910 bytes.
- SHA-256: `a3cbf2731591d59fa86f7584415a3e60d2fcc1c83c64b87be3492dc8d1cfa5cd`.
- Existing ARM EABI enum warnings remain; no new link failure is present.
- v3.2 is not yet physical-Vita verified. Do not treat the build result as proof that the full intro
  or complete main menu now renders correctly on hardware.

## Next physical-Vita check

1. Confirm fresh log starts `MELEE_VITA_GAME_BOOT v3.2`.
2. Opening must progress beyond frame 0 without `GAME_OPENING_FAIL ... marker type 0x3f`; capture the
   first later-frame failure if one appears.
3. If the intro completes or is skipped, Title must show the missing frame-controlled elements and
   still accept START.
4. GM_MENU must show Panel/ConTop/Cursor above `MenMainBack`, not the background-only v3.1 screen.
5. Return the fresh `runtime.log` plus screenshots. The next code step is then full
   `mnMain_Scene_OnEnter`/OnFrame integration and real menu navigation, not another diagnostic UI.

## Historical v2.6 development assessment

The former dirty GS_MEMCARD entry attempt did not link. It now links and ARM tests execute
original gm_Scene_MemCard_OnEnter successfully, loading LbMcGame, NtMemAc, NtMsgWin and
SdMsgBox through the port DVD backend. New code supplies scalar SDK matrix/vector functions,
original CObj/WObj runtime, scene heap/GObj setup and typed NtMsgWin camera scalar conversion.
The SIS arena must be recreated after the main heap reset; this fixes an observed ARM panic.
All prior ARM renderer/texture tests pass after the new scene entry.

This remains a bounded entry test. The original scene manager, GS_MEMCARD OnFrame, opening movie
and title screen are not running. The diagnostic renderer is preserved. Existing AXDriver/Synth
initialization is a partial boot adapter with no audible mixer/output; aux effects/bank setup is
still bypassed. Later gmmain probes do not imply that this earlier audio gap has been completed.

The current build/vita/runtime.log starts with WiiCompiled, not Melee. It is unrelated evidence
and has not been overwritten or accepted as a new Melee hardware run. v2.5 hardware results below
are historical records from the existing status, not reconfirmed by this log.

OnFrame dependency audit: build/vita/boot-frontier/memcard-frame-undefined.txt lists reachable
missing CARD async services, GX text/TEV drawing and further runtime/game dependencies. Do not
replace them with success stubs to claim game boot. v2.6 package/hardware results follow.

## v2.6 validation and installation

- Build exit 0: build/vita/build-v2.6.log. ARM EABI enum warnings remain.
- ARM and asset checks exit 0: build/vita/checks-v2.6.log. 861 archives pass, 33 are
  classified nonstandard; all 357 menu textures and previous GX replay tests pass.
- Additional independent original-disc camera test passes: build/vita/arm-camera-v2.6.log.
  Eye=(0,0,64), projection=perspective, viewport=640x480; view matrix and near/far match.
- Scene entry stats: stages=7, GObjs=2, main heap free=18839168, state=0,
  selected next mode=24 (GM_OPENING_MV). Selecting this ID does not enter/play the movie.
- One original message GObj process executes. Original GS_MEMCARD OnFrame does not execute.
- Package CRC, eboot byte parity and all eight data-file manifest hashes pass.
- No v2.6 physical-Vita result is available. Current checkout HEAD is 0b6cda51e;
  pre-existing dirty work was preserved, and no commit/push was performed in this run.

Install build/vita/SmashMeleeVita-v2.6.vpk (SMEL00001, version 00.18), and extract
build/vita/SmashMeleeVita-boot-assets.zip under ux0:data/. This ZIP includes the previous
MnMaAll.usd plus seven boot archives covering both supplied locales. The previous menu-only
ZIP is insufficient. Missing required files log GS_MEMCARD_ASSET_MISSING and stop entry.

Expected fresh log markers: MELEE_VITA_GS_MEMCARD v2.6; GS_MEMCARD_BEGIN HEAP,
SIS_GOBJ, ON_ENTER; GS_MEMCARD_CAMERA_NATIVE_PASS; GS_MEMCARD_BEGIN COMPLETE;
GS_MEMCARD_ENTER_PASS stages=7 gobjs=2 projection=1 viewport=640x480 state=0 next=24
(with heap_free between the viewport/state fields); then existing GX replay/PRESENT_120 markers.
The screen remains explicitly labelled GX preview. It is not the original memory-card UI.
Retrieve ux0:data/SmashMeleeVita/runtime.log after launch. SELECT+START exits.

- `build/vita/SmashMeleeVita-v2.6.vpk`: 180674 bytes; SHA-256 `4cc36e89d24e2fbf805cd4d314bfe6dcbfc558841e38b518a028d099b5f48fd6`.
- `build/vita/SmashMeleeVita-boot-assets.zip`: 592391 bytes; SHA-256 `dc5438711f882cc2e77e520b35b3d6a53c9f5a310072e4753241a97e93bfc8db`.
- `build/vita/melee_vita`: 1209744 bytes; SHA-256 `2d077b89f3b0d68f140eaf6b5b7355b61f81173dcfa87b721224e33f386f1bb1`.
- `build/vita/eboot.bin`: 184215 bytes; SHA-256 `35b16204dba9d94ccc11751f439e4d81c335d8f0ee905921201321b514dc4f80`.

## Historical v2.6 remaining phases toward actual game boot

1. Physical Vita validation of the new heap reset, archives, camera and GObj entry.
2. Complete GS_MEMCARD OnFrame dependencies and native typed message model/animation/SIS
   consumption, then its actual scene scheduler/render callbacks and memory-card behavior.
3. Close the earlier audio gap: AX effects/banks, decoding/mixing and Vita audio output.
   Current AX control state alone cannot produce sound.
4. Execute original scene exit/GM_BOOT transition, opening movie path (THP dependencies),
   title state and its real per-frame rendering/input. General GX drawing remains incomplete.
5. Validate visible title/menu and responsive input on real Vita with a fresh matching log.
   That establishes actual game boot; match/gameplay is a subsequent milestone.

The partial-link audit has 226 unresolved entries including libc/platform functions normally
supplied by other link inputs. This is an audit inventory, not 226 independent missing features.
Immediate unimplemented groups include CARD async operations and general GX text/TEV submission.

## Previous recorded milestones (historical)

Last hardware-verified renderer milestone: **v2.5**. Physical Vita now executes all 96/96 captured
`MenMainBack` draw commands / 324/324 source triangles including the command-7 two-TObj offscreen GXM
path first proven in v2.4. v2.5 additionally captures and applies the effective HSD PE state for every
draw: 81 normal alpha-blend commands, 15 `SRC_ALPHA + ONE` additive commands, LEQUAL on 96/96 commands,
no depth writes, and the original PObj culling split (8 none / 88 back-face). The hardware log reports
`HSD_GX_PE_CAPTURE_PASS`, `HSD_GX_REPLAY_READY_PASS ... pe=PASS`, then a real 96-command submit with
46 source triangles removed by GX-clockwise back-face culling and 280 output triangles. The original
game/menu still does not boot; the current renderer is still a diagnostic replay of the original
`MenMainBack` frame-0 HSD graph rather than execution of the original game scene loop.

## Current state

v1.5 is now verified on physical Vita. The original HSD component order completed through OS, VI,
GX, DVD, ID, retrace, object allocators and logging (`stages=0x3ff`), the VI/GX black-boot state
validated, upstream `memory.c`/`objalloc.c` supplied the constructor heap, the existing archive and
`HSD_JObjLoadJoint` paths remained green, the scene reached `PRESENT_120`, and the app exited cleanly
at frame 1335. The screenshot still shows the diagnostic background preview; component PASS does not
claim that the original menu renderer is visually correct.

v1.6 advances the exact original `gmmain.c` continuation after `HSD_InitComponent`:
`GXSetMisc(GX_MT_XF_FLUSH, 8)`, the original seed assignment, then the real
`lbAudioAx_8002838C`. The audio function executes its original `ARInit -> ARQInit -> AIInit` prefix
and original bank-size calculations, then stops explicitly before `AXDriver_8038E498` /
`HSD_SynthInit`. No audible output is claimed yet.

v1.7 starts the renderer transition using the architecture of ACGC-Vita-Port as a reference, without
replacing any already hardware-verified OS/HSD/DVD/audio adapter. A bounded native GX command buffer
now receives calls from the upstream rigid `pobj.c` path: `GXSetArray`, `GXSetVtxDesc`,
`GXSetVtxAttrFmt`, rigid position-matrix state and `GXCallDisplayList`. On ARM the authentic
`MenMainBack` runtime graph captures 86 display lists into 96 draw commands / 552 source vertices /
324 triangles, exactly matching the independent DAT decoder's 324-triangle count. This milestone
captures state/geometry only.

v1.8 adds the first actual replay submit from that queue. The supported fail-closed subset is 75/96
commands and 199/324 source triangles, all single-texture UV materials. One command is excluded for
multitexture, 10 for custom TEV, and 10 for unsupported colormap/alphamap modes. Physical Vita logged
`HSD_GX_REPLAY_READY_PASS` and `HSD_GX_REPLAY_SUBMIT_PASS`; no draw-command matrix-index mismatch
occurred.

v1.9 adds the original HSD TObj UV texture matrix to that replay. The first two rows are derived from
the same `MakeTextureMtx` semantics used upstream: repeat/scale, mirrored-T translation correction,
Euler texture rotation and translation. Raw GX `TEX0` values are therefore no longer submitted as if
the post-texture matrix were identity. A new independent world-bounds test also proves that captured
GX vertices transformed by the rigid JObj matrices reproduce the materialized scene bounds within
0.01 units, eliminating the model hierarchy as the current distortion suspect. v1.9 also defers real
GX PE/Z handling: the transitional vita2d replay now uses the same neutral `z=0.5` as the known-visible
comparison renderer instead of encoding camera depth before `GXSetZMode`/PE state has been ported.

v2.0 maps the original TObj sampler addressing directly to the GXM texture used by vita2d:
`GX_CLAMP -> SCE_GXM_TEXTURE_ADDR_CLAMP`, `GX_REPEAT -> ...REPEAT`, and `GX_MIRROR -> ...MIRROR`
independently for S/T. Texture-cache identity includes wrap state so the same image can safely be used
with different GameCube addressing. Invalid wrap values fail closed. The same milestone also resolves
all ten previously skipped single-texture HSD colormap/alphamap commands by baking the exact
`TObjMakeTExp` one-stage operations into per-material RGBA textures (NONE/PASS, masks, BLEND,
MODULATE, REPLACE, ADD and SUB). Texture-cache identity therefore also includes material RGBA,
TObj flags and blending. The supported subset is now 85/96 commands and 219/324 triangles; only one
multitexture command and ten custom `HSD_TObjTevDesc` commands remain fail-closed. Physical Vita
exposed that direct GXM MIRROR is rejected for these vita2d replay textures: commands 82, 84 and 86-93
all log `GX_REPLAY_TEXTURE_FAIL`, followed by `HSD_GX_REPLAY_READY_FAIL texture_failures=10`.

v2.1 replaces direct GXM MIRROR with a texture-domain mirror bake. A mirrored axis allocates a texture
twice as large, stores `original | reflected`, uses GXM REPEAT, and scales the corresponding post-HSD
UV coordinate by 0.5. If both axes mirror, both dimensions are doubled. Direct GX_REPEAT/GX_CLAMP
remain mapped to GXM. The supplied physical-Vita screenshot shows this replay active at 85/96 commands,
219/324 triangles and 34 textures, so the mirror-bake path is visually hardware-verified. The attached
runtime log was stale v2.0 and is not used as v2.1 marker evidence.

v2.2 captures the complete raw 32-byte `HSD_TObjTev` state in each GX material command and classifies
the ten previously skipped custom descriptors against upstream `MakeColorGenTExp`. Four descriptors are
inactive (`active=0`) and are exact no-ops. The remaining six all use the same active graph:
`GX_TEV_ADD`, zero bias, scale 1, clamp enabled, color inputs `TEV0.rgb`, `KONST.rgb`, `TEXC`, `ZERO`,
with no custom alpha stage. The replay therefore precomputes exactly
`RGB = lerp(TEV0.rgb, KONST.rgb, texture.rgb)` per texel before the existing HSD colormap/alphamap
operation. The matcher is intentionally narrow; any other custom TEV graph remains fail-closed. ARM
classification is 95/96 commands and 322/324 triangles, leaving only one multitexture command
(two triangles) unsupported. Physical Vita v2.2 then confirms this exact state: 42 replay textures,
zero texture failures, 95 submitted commands, 322 input triangles expanded/clipped to 324 output
triangles, and `PRESENT_120`.

v2.3 characterizes that final multitexture draw directly from the upstream runtime object graph instead
of guessing its TEV semantics. It is command 7, two triangles, two UV TObjs with identity texture
matrices and inactive custom TEV. Stage 0 is HSD `MODULATE` for RGB and alpha using an 80x128 GX I4
texture; stage 1 is RGB `MODULATE` with alpha preserved using a 64x4 indexed texture. Both use linear
filtering. The decoded first texture contains all 16 alpha levels from 0 through 255, while the second
is alpha 255. Consequently neither a CPU-composited source texture nor a multiply pass directly onto
the already-rendered framebuffer is exact: the two independently filtered samples must be combined
before final framebuffer blending.

The v2.3/v2.4 replay implementation creates a 960x544 A8B8G8R8 offscreen target for this one draw. Stage 0 uses
a GXM fragment program with REPLACE blending; stage 1 uses `DST_COLOR` multiplication for RGB while
preserving destination alpha. The resulting pre-PE image is then inserted at command 7's original draw
position and receives the existing final vita2d blend exactly once. Both fragment programs reuse the
texture/tint GXP binaries and shader patcher already linked by libvita2d; no shared VitaSDK/library,
SUPRX, shader compiler or dependency is modified. The matcher remains fail-closed and only accepts the
observed two-TObj graph. ARM classification is exactly 96/96 commands and 324/324 triangles with all
skip counters zero. The v2.3 hardware run exposed that the replay-side `command_supported()` still
rejected the preserved `UNSUPPORTED_MULTITEX` provenance bit before the exact matcher, so the offscreen
target was never initialized despite the successful classifier. v2.4 clears that bit only after the
same fail-closed matcher succeeds. The actual offscreen GXM execution still requires physical-Vita proof.

VI/GX support is deliberately limited to initialization state: two 640x480 XFBs, black YUYV
initial copy, retrace callbacks and XFB NEXT-to-DISPLAY transitions, FIFO memory reservation,
vertex-state reset and eight initialized lights. The XFB is not presented as a game framebuffer;
FIFO memory is not a working GX command processor. Unsupported non-black EFB copies fail.
Draw-done is synchronous for these CPU operations; actual GPU submissions will need real fences.
Retrace callbacks are pumped by VIWaitForRetrace, not an asynchronous VI service.
General arbitrary TEV/multitexture support beyond the exact MenMainBack graphs, PE state,
perspective-correct native 3D submission, AX/HSD Synth, a Vita audio-output sink and the original game
scene loop are still missing.

The exact continuation boundary is now **inside `lbAudioAx_8002838C`, immediately before
`AXDriver_8038E498`**. The earlier gmmain probes remain bring-up helpers, not execution of the
complete original main function.

Repository: https://github.com/robin994/SmashMeleeVita (verified fork of doldecomp/melee).
Local branch: `vita-port`; origin is the fork, upstream is doldecomp/melee.
Upstream snapshot: `05a1394faea2aac458e4bdd030621d8a5631ae62`.
The port additions remain uncommitted/unpushed. Selected upstream Melee/HSD files are now compiled
directly into the VPK behind Vita-specific bring-up defines; unsupported runtime paths remain
fail-closed. No reset, clean, commit or push has been performed during this milestone.

## Original disc and data

- User ISO remains in `orig/GALE01/Super Smash Bros. Melee (USA) (En,Ja) (v1.02).iso`.
- Disc header verified: GALE01, revision 2, GameCube magic c2339f3d.
- DOL extracted to `orig/GALE01/sys/main.dol`: 4,425,184 bytes.
- DOL SHA-1 matches upstream exactly: `08e0bf20134dfcb260699671004527b2d6bb1a45`.
- 1,209 disc files extracted into `orig/GALE01/files/`.
- `orig/GALE01/extraction-manifest.json` records size/SHA-256 for each output.
- All original data, ISO, generated files and local deployment archives are ignored by Git.
  The only tracked file below orig remains `orig/GALE01/sys/.gitkeep`.

## Implemented this milestone

- `src/melee/gm/gmmain.c`: real Vita bring-up entry preserves the original early boot ordering
  (`OSInit`, `VIInit`, `DVDInit`, `PADInit`, `CARDInit`, alarm/debug-level setup and arena logic).
- `src/melee/lb/lbfile.c`: original file API is linked. `HSD_DevComRequest` DVD-to-RAM requests are
  serviced synchronously from `ux0:data/SmashMeleeVita/files`; unsupported DevCom destinations fail.
- `src/melee/lb/lbarchive.c`: original `lbArchive_InitializeDAT` is linked and exercised end-to-end.
- `src/sysdolphin/baselib/archive.c`: upstream `HSD_ArchiveParse` now reads original big-endian DAT
  headers/relocation metadata correctly on ARM. This does not remove the need for typed conversion
  of scalar descriptor fields before passing arbitrary archive roots to runtime constructors.
- Selected upstream HSD object runtime (`class`, `object`, `id`, `jobj`, `dobj`, `mobj`, `pobj`,
  `tobj`) is linked. `HSD_JObjLoadJoint` constructs the validated `MenMainBack` graph on ARM/Vita.
- `src/sysdolphin/baselib/initialize.c`: v1.5 allocates original XFB/FIFO reservations and
  invokes original HSD_InitComponent. Stage traces and assertions go to runtime.log.
- Original video/state/TEV/light/list/animation/matrix/RObj/shadow/display initialization
  units are linked with the limited boot adapter in vita/gx_boot_vita.c.
- `vita/gx_boot_vita.c`: v1.6 adds the reached `GXSetMisc` state. `GX_MT_XF_FLUSH` is recorded and
  validated at the original value 8; unsupported tokens remain fail-closed.
- `src/melee/lb/lbaudio_ax.c`: the original `lbAudioAx_8002838C` is linked. The Vita probe executes
  its real AR/ARQ/AI and bank-sizing prefix, records the results and returns at the explicit
  AXDriver/HSD Synth frontier rather than substituting successful AX stubs.
- `vita/audio_boot_vita.c`: bounded 16 MiB ARAM model with original user base `0x4000`, aligned LIFO
  `ARAlloc`/`ARFree`, synchronous 32-byte DMA, ARQ state and AI DMA/rate/stream state. The ARM test
  performs a real write/read ARAM DMA round-trip. This is control/memory bring-up, not `SceAudioOut`.
- `vita/gx_capture_vita.c`: first bounded GX translation/capture layer, modeled after the command
  buffering strategy proven by ACGC-Vita-Port but implemented locally for Melee/HSD. It tracks vertex
  arrays/descriptors/formats, cull and matrix state, parses the original big-endian GX display-list
  stream and stores decoded source vertices plus draw-command metadata. Queue overflow/malformed or
  unsupported input fails instead of being silently skipped.
- `vita/gx_replay_vita.c`: first physical-Vita command replay. It expands captured GX primitives,
  applies per-command rigid HSD model matrices, original HSD camera projection, near/far clipping,
  decoded TObj textures and the supported HSD UV post-matrix. Unsupported TEV/multitexture/material
  modes are excluded rather than approximated as successful GX support.
- `src/sysdolphin/baselib/pobj.c`: a Vita-only rigid capture entry reuses the original private
  `setupArrayDesc` / `setupVtxDesc` / simple display-list path. It accepts only the already validated
  rigid `POBJ_SKIN` subset; shape animation, envelope and shared-skin remain fail-closed and are not
  linked merely to satisfy the capture milestone.
- `vita/gc_runtime_vita.c`: Vita implementations now cover arena allocation, timer/ticks, vblank,
  basic interrupt-state semantics, DVD/file I/O, and a bounded 32-byte-aligned OS heap allocator
  with split/free/coalescing used to create HSD's audio and main heaps.
- The GameCube HSD/GX descriptor island is compiled with `-fno-short-enums`; this is required because
  Vita ARM EABI short enums otherwise change layouts such as `HSD_TObjDesc` and corrupt pointers.

- `vita/tools/extract_disc.py`: validated FST/DOL extraction, range/name checks, identical-file
  verification and refusal to overwrite differing output.
- `vita/hsd_data.c`: immutable big-endian archive view, validated headers, relocation fields,
  symbols and ranges. Relocated zero offsets are distinct from null pointers. Handles original
  unaligned relocation fields and one-past-data references; dereferencing beyond data is rejected.
- `vita/hsd_scene.c`: iterative traversal of named Joint -> DObj -> MObj -> TObj graphs, deduplicated
  by descriptor/type with bounded work and cycle handling. Excludes particle/spline union data.
- `vita/hsd_scene.c`: typed static HSD camera reader. `ScMenMain_cam_int1_camera` resolves to a
  perspective camera with eye `(0,0,51)`, interest `(0,0,0)`, near/far `1/5000`, FOV `41.539°`
  and aspect `4:3`. Static WObj RObj indirections are fail-closed until adapted.
- `vita/hsd_native.c`: immutable big-endian -> native host-endian descriptor conversion for the
  supported Joint/DObj/MObj/PObj/TObj/Image/Tlut/Material/PE/TEV subset. Shared descriptor offsets
  remain shared rather than being duplicated.
- `vita/gx_texture.c`: tiled I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CI4, CI8, CI14X2 and CMPR
  to linear RGBA; palette decoding; partial-tile clipping and destination-stride handling.
- `vita/asset_viewer.c`: on-device loading from ux0:data, 12-texture page cache, GPU completion
  before freeing old textures, page controls, explicit failures and transition logs. The static
  scene is projected with the original HSD camera and triangles are clipped against near/far before
  vita2d submission. The preview preserves the original 4:3 camera aspect inside the Vita UI.
- Shared host/Vita implementation plus ASan/UBSan tests and ARM ELF parity tests.
- Separate, hash-verified local asset ZIP; original archive is decoded on device, not on the PC.
- Retained upstream HSD_Rand/PADClamp and native PADInit/PADRead diagnostics from the first build.

The custom reader/converter remains necessary even though upstream `HSD_ArchiveParse` now runs:
archive relocation alone does not endian-convert descriptor scalars. A typed native descriptor graph
is therefore still used to enter `HSD_JObjLoadJoint` safely. AnimJoint frame 0 and the supported
MatAnim/TexAnim frame-0 channels are decoded; ShapeAnim for this background is empty. General
animation playback, complete RObj/envelope/shape paths, general extern-symbol linking, GX TEV,
multi-texture state, audio, the original menu state machine and gameplay remain incomplete.

## Verified evidence

- `make -f Makefile.vita`: ARM compile/link, SELF and VPK creation passed with GCC 15.2.0.
- `make -f Makefile.vita asset-check`: passed after the v1.9 changes.
- `make -f Makefile.vita arm-check`: passed after the v1.9 changes.
- Extractor tests: valid FST, rejected bad ranges/names/directories, original preservation,
  identical re-read and symlink refusal.
- C format/bounds tests passed under AddressSanitizer and UndefinedBehaviorSanitizer:
  all 11 supported GX formats, CMPR interpolation/transparency, padding/partial tiles, palette
  bounds, truncated input, archive references, unaligned offsets, cycles and malformed metadata.
- 861 HSD archive containers validated; 0 rejected. 33 DAT/USD files have a different container
  header and were classified as nonstandard, not claimed as parsed.
- MnMaAll.usd: 2,155,311 bytes, 234 public symbols, 11,507 relocation entries; 57 named joint roots.
- All 357 static menu textures decoded under sanitizers: I4=194, I8=43, IA4=91, RGB565=6,
  RGBA8=2, CI4=15, CI8=5, CMPR=1.
- Inspected host contact sheet: recognizable menu text, icons, controller illustrations and sprites.
  This is CPU decode evidence, not a PS Vita screenshot.
- ARM ELF tests: HSD_Rand known answers and PAD mapping/clamping passed. All 357 descriptor records
  match host output, input archive remains unchanged, and one real texture for each of the eight
  formats present matches host RGBA bytes exactly.
- Native descriptor parity: `MenMainBack_Top_joint` converts to 102 Joint, 86 DObj, 86 MObj,
  86 PObj, 87 TObj, 87 ImageDesc, 86 Material, 14 PEDesc, 9 TEV and 2 Tlut descriptors with
  zero unsupported nodes. The 86 PObjs share nine original vertex-descriptor lists.
- Camera host/ARM parity: type 1 perspective, viewport 640x480, eye `(0,0,51)`, interest origin,
  up `(0,1,0)`, near/far `1/5000`, FOV `41.5389977`, aspect `1.33333302`.
- Camera visibility classification on the decoded geometry is identical on host and ARM:
  322 triangles fully in range, 2 crossing a clip plane, 0 fully behind and 0 beyond far.
- Scene UV range is `U -0.001953125..1.00195312`, `V -0.001953125..1.0`; this is normalized
  texture space with small half-texel offsets, so the v0.3 distortion was not caused by a missing
  pixel-to-normalized UV conversion.
- Real Vita v0.3 hardware evidence: `HSD_GEOMETRY_PASS` reported 102 joints, 86 DObjs/PObjs,
  86 meshes, 324 triangles and 86 textured meshes; all 86 scene textures were allocated using
  463,872 bytes; the process reached 3,158 frames and exited cleanly through START. The supplied
  screenshot showed severe diagonal stretching/overlap because v0.3 ignored the original camera.
- Real Vita v1.3 hardware evidence: `GMMAIN_BOOT_PREFIX_PASS` completed the selected original
  `gmmain.c` prefix; `HSD_ARCHIVE_UPSTREAM_PASS` reported 2,155,311-byte MnMaAll, 2,100,440 bytes
  of data, 11,507 relocations and 234 public roots; `LBFILE_DVD_PASS` read the same file through the
  Vita DVD backend; `LBARCHIVE_DAT_PASS` resolved `MenMainBack_Top_joint` at data offset `0x6828`.
- The same v1.3 hardware log reported `HSD_RUNTIME_JOBJ_PASS` with 102 JObj, 86 DObj/MObj/PObj and
  87 TObj through upstream `HSD_JObjLoadJoint`, all 86 scene textures ready (463,872 bytes), and
  `PRESENT_120`. No v1.3 boot-path failure marker was observed.
- ARM Unicorn independently passes upstream `HSD_ArchiveParse` with 11,507 relocations / 234 public
  roots / root offset `0x6828`, plus upstream `HSD_JObjLoadJoint` with 102 JObj / 86 DObj / 87 TObj.
- Real Vita v1.4: HSD_INIT_PREFIX_PASS stages=00000007, main_free=20447200,
  audio_free=524256, physical=25165824; archive/constructor/texture checks also pass.
- Real Vita v1.5: `HSD_COMPONENT_INIT_PASS` reports main_free=18,956,256, audio_free=524,256,
  next_arena=18,956,288 and stages=0x3ff. `VI_GX_BOOT_PASS` reports 640x480, retraces=3, copies=1,
  flushes=2 and all eight lights. `HSD_RUNTIME_HEAP_PASS` reports 55,136 bytes consumed through
  upstream `memory.c`/`objalloc.c`; `HSD_RUNTIME_JOBJ_PASS` still constructs 102 JObj, 86 DObj and
  87 TObj. All 86 textures are ready, `PRESENT_120` is reached, and exit at frame 1335 is clean.
- v1.6 ARM: the original post-HSD gmmain prefix passes with `GXSetMisc=8`; ARAM is 16,777,216 bytes
  at base `0x4000`, and the DMA write/read round-trip passes. The original audio bank calculations
  produce 2,045,824 / 911,456 / 3,291,584 bytes, total 6,248,864 bytes. The link map contains
  `lbAudioAx_8002838C` but not `AXDriver_8038E498` or `HSD_SynthInit`, proving the stop boundary.
- Real Vita v1.6: `GMMAIN_POST_HSD_PASS`, `GX_MISC_PASS` and `AUDIO_PREFIX_PASS` are all observed.
  ARAM is 16,777,216 bytes, ARQ chunk is 4096, original bank total is 6,248,864 bytes and the
  RAM -> ARAM -> RAM round-trip passes. All previous HSD/archive/runtime markers remain green,
  86/86 scene textures are ready, `PRESENT_120` is reached and exit at frame 728 is clean.
- v1.7 ARM: the upstream rigid PObj path produces `86` display lists, `96` GX draw commands,
  `552` source vertices and `324` triangles with zero capture errors. The triangle count exactly
  matches the independent typed DAT decoder, providing a cross-check between two different paths.
- Real Vita v1.8: `HSD_GX_REPLAY_CLASS_PASS` reports 75 supported commands / 199 triangles;
  `HSD_GX_REPLAY_READY_PASS` allocates 13 replay textures (93,696 bytes) with zero failures;
  `HSD_GX_REPLAY_SUBMIT_PASS` submits all 75 commands, expands/clips 199 input triangles to 201 output
  triangles and reports `skipped_matrix=0`. `PRESENT_120` is reached and exit at frame 2098 is clean.
- v1.9 ARM: `mv_gx_capture_world_bounds` matches the independent materialized scene within 0.01 for
  all six bounds. This verifies the captured rigid JObj transform path independently of the GPU.
  The replay additionally applies the supported UV `MakeTextureMtx` equivalent before submit.
- Real Vita v1.9: `HSD_GX_REPLAY_READY_PASS` reports 13 textures / 93,696 bytes and 13 commands with
  HSD TObj UV matrices. `HSD_GX_WORLD_BOUNDS_PASS` matches the reference within epsilon 0.01 and
  `HSD_GX_REPLAY_SUBMIT_PASS` reports 75 commands / 199 input / 201 output triangles with
  `depth=deferred_z05`. The supplied screenshot proves that the GX replay is now visibly rendered on
  hardware; `PRESENT_120` is reached and exit at frame 628 is clean. Remaining long bands/streaks are
  rendering-state errors rather than absence of GPU submission or bad rigid JObj transforms.
- Real Vita v2.0: classification reaches 85/96 commands and 219/324 triangles, but commands 82, 84 and
  86-93 all log `GX_REPLAY_TEXTURE_FAIL`; these 10 failures match the 10 mirror-wrapped commands.
  `HSD_GX_REPLAY_READY_FAIL` reports textures=32, texture_failures=10, wrap_repeat=2, wrap_mirror=10,
  wrap_clamp=73. The comparison renderer remains operational and `PRESENT_120` is reached.
- v2.1 build/host/ARM: all previous checks remain green with 85/96 commands and 219/324 triangles.
  GX_MIRROR no longer requests GXM MIRROR directly: replay textures are expanded/reflected and use
  GXM REPEAT with per-axis UV scale 0.5.
- Real Vita v2.1 screenshot: GX replay is visibly active and reports 85/96 commands, 219/324 triangles
  and 34 textures. This is hardware proof for mirror-bake rendering. The attached runtime log is stale
  v2.0, so no v2.1 log-marker claims are made from it.
- v2.2 ARM: all 10 custom TObj TEV descriptors are captured byte-for-byte. Four are inactive no-ops;
  the six active descriptors share the exact `lerp(TEV0, KONST, TEXC)` color graph and no alpha graph.
  The exact CPU bake increases the supported subset to 95/96 commands and 322/324 triangles;
  `multitex=1`, `tev=0`, `mapmode=0`, `texcoord=0`, `vtxcolor=0`. All earlier runtime/texture parity
  checks remain green.
- Real Vita v2.2: fresh runtime log confirms `HSD_GX_REPLAY_READY_PASS textures=42
  texture_failures=0` and `HSD_GX_REPLAY_SUBMIT_PASS commands=95 input_triangles=322
  output_triangles=324`; the supplied screenshot visibly shows the 95/96 replay with the custom-TEV
  elements present.
- Real Vita v2.3: capture/classification reaches the full 96/96 commands and 324/324 triangles with all
  classifier skip counters zero, but `HSD_GX_REPLAY_READY_PASS` reports `multitex=none`, command
  `4294967295` and zero multitexture target bytes. The actual submit is still 95 commands / 322 input
  triangles. This proves the new graph classification on hardware but also proves the offscreen GXM path
  was not entered. The cause is the replay `command_supported()` ordering bug fixed in v2.4.
- Real Vita v2.4: the replay-side matcher clears `UNSUPPORTED_MULTITEX` only after the exact
  MenMainBack two-TObj graph is accepted. Hardware proves `multitex=PASS`, command 7, a 2,088,960-byte
  GXM offscreen target, zero texture failures and a real 96-command / 324-input-triangle submit.
- Real Vita v2.5: effective HSD PE state is captured and applied for every draw. Hardware proves
  81 normal alpha-blend commands, 15 additive `SRC_ALPHA + ONE` commands, LEQUAL on all 96 commands,
  no depth writes, 8 cull-none commands and 88 back-face-cull commands. Submit remains 96 commands;
  CPU GX-clockwise culling removes 46 source triangles and produces 280 output triangles. `PRESENT_120`
  still occurs and no matrix/texture/multitexture failure is observed.
- The final Vita link still emits the ARM EABI warning that objects are built with 32-bit enums while
  the output advertises variable-size enums. This has not produced an observed failure in v1.3, and
  all selected GameCube/HSD-facing sources are explicitly built with `-fno-short-enums`, but the
  warning remains an ABI item to eliminate rather than being treated as harmless indefinitely.
- ARM harness models libc services and vblank wait returns; it executes the port OS/VI
  state code. It does not emulate the Vita GPU, display timing or physical input. This is compiler/ABI evidence, not hardware proof.
- VPK integrity checks pass. Current packages include `eboot.bin`, `sce_sys/param.sfo` and the supplied LiveArea icon/pic/background/startup/template assets.

Reports: `build/vita/host/archive-audit.json`, `build/vita/host/menu-textures/report.json`.
Preview: `build/vita/host/menu-textures/contact-sheet.png`.

## Earlier ARM compiler baseline (unchanged sources)

1,038 / 1,182 C files compiled independently; 144 failed. For the game subtree, 842 / 908 compiled.
The original audit includes unselected SDK, MSL and MetroTRK units and omits some private includes;
it is neither the native game source manifest nor a link/runtime completion measure.
Evidence: `build/vita/audit/report.json` and per-file logs.

Remaining compile groups include int/bool callback mismatches, PowerPC assembly/intrinsics,
MSL/newlib FILE/va_list/setjmp differences, and DOL-generated font includes. Those includes can
now be extracted from the verified DOL; they have not yet been generated by this milestone.

## Historical next-work list (superseded by v2.6 phases above)

1. v2.4 and v2.5 are now hardware-proven. Treat MenMainBack frame-0 texture/material/PE coverage as
   closed for this diagnostic milestone; do not spend further iterations on the viewer unless a later
   original-game path exposes a concrete regression.
2. Replace the frame-0 diagnostic replay driver with the original
   MenMainBack animation/update/display call chain so that HSD AObj/TObj animation advances through the
   same runtime objects used for rendering rather than through the comparison viewer.
3. In parallel, continue `lbAudioAx_8002838C` from the exact next call, `AXDriver_8038E498`, by porting only its
   reachable AX/HSD Synth dependencies. Add a real Vita audio-output sink before claiming sound.
4. Continue the exact original `gmmain.c` sequence after audio: pad/event init, VI callbacks/black-off,
   `lbMemory`, `lbHeap`, `lbDvd`, `lbArq`, card/save, snapshot, THP, SisLib and `gmMainLib` setup.
5. Keep using `build/vita/boot-frontier/` and the final link map to distinguish immediate audio
   dependencies from transitive callbacks; do not satisfy AX/Synth with unconditional-success no-ops.
6. Link and execute `gm_801A4510()` only after those prerequisites are real enough to satisfy its game-mode
   initializers. The first meaningful "game boot" milestone is entering `GM_BOOT` through the original
   `runGameMode()` state machine, not showing another manually loaded menu asset.

## Historical v2.5 real-Vita check

The v2.5 physical-Vita check is complete. Hardware evidence includes:

- `MELEE_VITA_GX_PE v2.5`
- HSD_COMPONENT_BEGIN OS, VI, GX, DVD, ID, RETRACE, OBJECTS, LOG, COMPLETE in order
- HSD_LOG_INIT_PASS and HSD_COMPONENT_INIT_PASS with stages=000003ff
- `GMMAIN_POST_HSD_PASS ... stop=before_AXDriver_HSD_Synth`
- `GX_MISC_PASS xf_flush=8 dl_save_context=0 backend=VitaState`
- `AUDIO_PREFIX_PASS ... ar_base=00004000 ar_size=16777216 ... bank_total=6248864
  aram_dma=roundtrip_pass output=not_started stop=before_AXDriver_HSD_Synth`
- VI_GX_BOOT_PASS with width=640, height=480, the existing hardware retrace progression
  (v1.5 observed `retraces=3`), copies=1 and light mask ff
- Existing archive/DVD/constructor PASS markers, HSD_RUNTIME_HEAP_PASS and PRESENT_120
- `HSD_GX_CAPTURE_PASS ... display_lists=86 commands=96 vertices=552 triangles=324 ... errors=0`
- `HSD_GX_REPLAY_CLASS_PASS supported_commands=96 supported_triangles=324 ... skipped_multitex=0
  skipped_tev=0 skipped_mapmode=0 ...`
- ten `HSD_GX_CUSTOM_TEV` records; four must have `active=00000000` and six `active=40000077`
- `HSD_GX_WORLD_BOUNDS_PASS ...`
- `HSD_GX_PE_CAPTURE_PASS commands=96 blend_alpha=81 blend_additive=15 blend_other=0
  z_lequal=96 z_write=0 alpha_always=96 custom_pe=15 cull_none=8 cull_front=0 cull_back=88 cull_all=0`
- `HSD_GX_REPLAY_READY_PASS ... texture_failures=0 ...
  mirror_baked_textures=... sampler=GX_to_GXM_or_mirror_bake mapmode=HSD_TObj_TExp_CPU_baked
  custom_tev_baked=... custom_tev_noop=... custom_tev=MenMainBack_single_stage_CPU_baked
  multitex=PASS multitex_command=7 multitex_triangles=2/2 multitex_target_bytes=2088960
  multitex_backend=GXM_offscreen_replace_multiply pe=PASS blend_alpha=81 blend_additive=15
  z=LEQUAL z_write=0 cull_none=8 cull_back=88 ...`
- `HSD_GX_REPLAY_SUBMIT_PASS commands=96 input_triangles=324 output_triangles=280 ...
  depth=GXM_LEQUAL_no_write_neutral_z0 cull=GX_clockwise_CPU culled_triangles=46
  pe=HSD_PE blend=alpha_or_SRCALPHA_ONE alpha_write=off`

There are no `GX_MULTITEX_PROGRAM_FAIL`, `GX_MULTITEX_INIT_FAIL` or `GX_REPLAY_TEXTURE_FAIL` lines.
PS+START remains screenshot-safe; SELECT+START exits. This still does not mean original game boot.

## Format references

HSD layouts come from upstream `archive.h`, `jobj.h`, `dobj.h`, `mobj.h` and `tobj.h`.
The GameCube FST layout was cross-checked against
[Dolphin DiscIO](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/DiscIO/FileSystemGCWii.cpp).
GX layout/channel behavior and GameCube-specific CMPR interpolation were cross-checked against
[Dolphin TextureDecoder](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/TextureDecoder_Generic.cpp)
and its [DXTBlend utility](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/TextureDecoder_Util.h).

## Historical v2.5 artifacts

- `build/vita/SmashMeleeVita-v2.5.vpk`: 133,129 bytes; SHA-256 `f53d8807b5134a809705cdc6cdf1d33d64334e4f811aceaa237ca97a73df2ed7`.
- `build/vita/SmashMeleeVita-assets.vpk`: current v2.5 payload.
- `build/vita/SmashMeleeVita-menu-assets.zip`: 558,808 bytes; SHA-256 `a2bdf00d0789d1d62b146be72948a13f9effd023d6079852fed4b8abd493ed5c`.
- `build/vita/melee_vita`: 1,028,276 bytes; SHA-256 `8df4a3e67703f5105c943855929c1c82175f93cc367678a9381f3cda99d8e2c1`.
- `build/vita/eboot.bin`: 134,971 bytes; SHA-256 `a98e7685c5c70987fe0a8ec6ca586e1dbe533246b23b27c31a0697d8acad014c`.

v1.9 is hardware-verified with visible GX replay output. v2.0 hardware exposed the direct GXM MIRROR
failure. v2.1 verifies the mirror-bake replay; v2.2 verifies the custom-TEV expansion at 95/96 submitted
commands. v2.3 verifies 96/96 capture/classification but exposes that its offscreen multitexture path was
not entered. v2.4 hardware proves the command-7 offscreen GXM composition and a real 96/96 submit. v2.5
hardware proves the MenMainBack frame-0 PE subset: alpha/additive blending, LEQUAL/no-write and GX
back-face culling, with 96 commands submitted and 46 source triangles culled.
