# Full Menu Graph Strategy - PS Vita

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


## Active integration — supersedes v3.22 frontier policy

User direction: integrate missing original scene code; no artificial paused
runtime-frontier screen. The v3.22 frontier pause has been removed in source.
Work is in progress; do not install an intermediate package as a finished port.

Current edits: persistent outer scene dispatch, remembered menu parent/selection,
current GM state update, GObj pool reuse/scene destruction before native asset
release, original CSS/SSS callback bindings for VS plus nine Special variants
and Training. Original gmvs/gm_1884 sources are being linked rather than replacing
their missing state/functions with stubs. Native SSS rendering is still being
implemented; the temporary -80 return must be replaced before completion.

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




Date: 2026-09-10
Target baseline: MELEE_VITA_GAME_BOOT v3.21 / Vita package version 00.31

## Goal

The next menu milestone is not another one-screen release. The goal is to connect the complete reachable Melee menu graph first, using the original HSD menu state machines and assets wherever they already exist, and only then start the bug-fix/fidelity pass.

"Menu graph complete" means every selectable item has a deterministic edge: it opens the correct original submenu/panel, returns to the correct parent, or exits GM_MENU with the correct GM_* target. A terminal gameplay mode may still stop at the existing Vita runtime frontier, but the menu edge itself must never be a no-op, branch-to-NULL, accidental app exit, or ambiguous route.

## Documentation rule

PORTING_STATUS.md is the canonical live status file for this port.

Every future change that affects menu routing, scene transitions, asset conversion, rendering, linker/runtime frontiers, hardware results, or a testable VPK must update PORTING_STATUS.md in the same iteration. A build is not considered ready for hardware testing until the status file describes what changed, what is proven, what is still bounded, and which log markers are expected.

## Current baseline

The source checkout already contains the broad v3.21 menu integration work:

- MN_SUBMENU_THINK(fn) is active on Vita instead of forcing submenu think callbacks to NULL.
- MN_PLATFORM_PREP(call) and MN_EXTERNAL_ACTION(..., call) execute their original calls.
- Regular Match uses the real mn_8022CC28, so Classic/Adventure/All-Star routing is no longer input-dead.
- The Vita target includes the original menu modules for Event, Multi-Man, Rules, Item Switch, Stage Switch, Records/Diagram pages, Settings, Data, Snapshot, Gallery, Sound/Sound Test, Language, Deflicker, Rumble and Name Entry.
- MnMaAll.usd has a lazy Vita archive proxy. Requested JObj, AnimJoint and MatAnimJoint symbols are converted on demand to ARM-native descriptors; optional ShapeAnim is currently allowed to be absent.
- Menu GX capture is dynamic: after the known background/panel/content roots, extra live HSD JObjs are discovered from HSD_GObjGXLinkHead and captured into the same vitaGL replay.
- Classic is hardware-proven through original CSS exit and first-match StartMeleeData preparation. Actual GS_VS fighter/ground/HUD execution remains a separate gameplay-runtime frontier.
- The broad menu source set links and packages successfully locally; full physical-Vita navigation coverage is not yet proven.

## Graph inventory

The implementation treats the graph in three classes: internal MENU_KIND_* transitions, embedded/external menu panels that remain inside GM_MENU, and terminal GM_* transitions.

| Parent path | Selection / child | Expected target |
| --- | --- | --- |
| MAIN | 1-P Mode | MENU_KIND_1P |
| MAIN | VS Mode | MENU_KIND_VS |
| MAIN | Trophies | MENU_KIND_TOY |
| MAIN | Options | MENU_KIND_SETTINGS |
| MAIN | Data | MENU_KIND_DATA |
| 1-P | Regular Match | MENU_KIND_REG |
| 1-P / Regular | Classic | GM_CLASSIC |
| 1-P / Regular | Adventure | GM_ADVENTURE |
| 1-P / Regular | All-Star | GM_ALLSTAR |
| 1-P | Event Match | original mnEvent panel -> GM_EVENT |
| 1-P | Stadium | MENU_KIND_STADIUM |
| Stadium | Target Test | GM_TARGET_TEST |
| Stadium | Home-Run Contest | GM_HOME_RUN_CONTEST |
| Stadium | Multi-Man Melee | original mnHyaku / MENU_KIND_MULTI_VS |
| Multi-Man | 10-Man | GM_10MAN_VS |
| Multi-Man | 100-Man | GM_100MAN_VS |
| Multi-Man | 3-Minute | GM_3MIN_VS |
| Multi-Man | 15-Minute | GM_15MIN_VS |
| Multi-Man | Endless | GM_ENDLESS_VS |
| Multi-Man | Cruel | GM_CRUEL_VS |
| 1-P | Training | GM_TRAINING |
| VS | Melee | GM_VS |
| VS | Tournament | GM_TOURNAMENT |
| VS | Special Melee | MENU_KIND_SPECIAL |
| Special Melee | Camera | GM_CAMERA_MODE |
| Special Melee | Stamina | GM_STAMINA_VS |
| Special Melee | Super Sudden Death | GM_SUPER_SUDDEN_DEATH_VS |
| Special Melee | Giant | GM_GIANT_VS |
| Special Melee | Tiny | GM_TINY_VS |
| Special Melee | Invisible | GM_INVISIBLE_VS |
| Special Melee | Fixed Camera | GM_CAMERA_VS |
| Special Melee | Single Button | GM_SINGLE_BUTTON_VS |
| Special Melee | Lightning | GM_LIGHTNING_VS |
| Special Melee | Slow-Mo | GM_SLOMO_VS |
| VS | Rules | original Rules panel (MENU_KIND_RULES) |
| Rules | Extra Rules | MENU_KIND_RULES_EXTRA |
| Rules | Item Switch | MENU_KIND_RULES_ITEMS |
| Rules | Stage Switch | MENU_KIND_RULES_STAGE |
| VS | Name Entry | MENU_KIND_NAME_ENTRY |
| Trophies | Gallery | GM_TOY_GALLERY |
| Trophies | Lottery | GM_TOY_LOTTERY |
| Trophies | Collection | GM_TOY_COLLECTION |
| Options | Rumble | MENU_KIND_SETTINGS_RUMBLE |
| Options | Sound | MENU_KIND_SETTINGS_SOUND |
| Options | Display | MENU_KIND_DISPLAY |
| Options | Language | MENU_KIND_SETTINGS_LANG |
| Options | Erase Data | MENU_KIND_SETTINGS_ERASE |
| Data | Snapshots | MENU_KIND_DATA_SNAP |
| Data | Archives / Gallery | MENU_KIND_DATA_ARCHIVES |
| Data | Sound Test | original Sound Test panel |
| Data | Records | MENU_KIND_RECORDS |
| Records | VS Records | MENU_KIND_RECORDS_VS |
| Records | Bonus Records | MENU_KIND_RECORDS_BONUS |
| Records | Misc Records | MENU_KIND_RECORDS_MISC |
| Data | Special / Notices | MENU_KIND_DATA_SPECIAL |

Hidden/unused MENU_KIND_* entries remain in the enum/table for compatibility but are not counted as reachable graph nodes unless an original retail path exposes them.

## Architecture

### 1. Separate menu exit state from Title state

Do not reuse the Title bridge state as generic scene exit storage. GM_MENU gets a dedicated MenuExitData/route object owned by the menu runtime. gm_GetCurrentSceneExitData() must return data for the currently executing scene boundary, not a Title-specific scratch variable.

The menu route result preserves target GM_*, current and parent MenuKind, current selection, controller port where required, and whether the transition is internal, external-panel, terminal-mode, or back/return.

### 2. Keep one persistent original mnMain shell

The original mnMain scene remains the owner of the menu graph. Internal transitions use the original mn_803EB6B0[] think callbacks and mn_8022B3A0() animations instead of separate Vita UI implementations. No Vita-specific duplicate menu is created for a retail menu that already has an original mn* implementation.

### 3. Make external panels first-class graph nodes

Rules, Item Switch, Stage Switch, Name Entry, Event, Multi-Man, Rumble, Sound, Display, Language, Erase Data, Snapshot, Gallery, Sound Test, Records and Info panels are graph nodes with explicit enter/active/exit/return behavior, not one-shot callbacks.

Opening one must suspend or replace the parent GObjs as the original code expects; Back must restore the exact parent menu and previous selection rather than reconstructing MAIN.

### 4. Lazy MnMaAll conversion is the shared asset layer

Keep the current lazy proxy and expand it rather than writing per-menu converters. The proxy must support JObj, AnimJoint, MatAnimJoint/TexAnim, ShapeAnim when a reachable screen requires it, raw pointer/string tables such as Name Entry lists, and camera/light/fog descriptors if requested.

Every conversion failure logs the symbol, data offset, conversion kind and owning menu. Optional data may be skipped only when the original caller accepts NULL.

### 5. Dynamic HSD capture, not hardcoded roots

Each frame, capture all relevant live menu JObjs in stable GX-link/render-priority order. The known Back/Panel/Content roots remain ordered anchors, but newly created external-panel GObjs are captured automatically.

Deduplicate roots by HSD object pointer and never capture camera/light/SObj objects as JObj data. SIS/SObj text is a separate renderer island: navigation must not be blocked on it, but every screen that depends on text remains tagged until its vitaGL text path is complete.

### 6. Convert non-HSD big-endian tables before use

GmEvent.dat and any other data-only DAT sections cannot be handed directly to ARM code merely because archive lookup succeeds. Build typed converters for reachable tables/pointers and keep the converted data alive for the lifetime of the owning menu/mode.

Event Match is the first mandatory data-table case. Invalid/unconverted pointers fail with an explicit log marker rather than being dereferenced.

### 7. General terminal-mode dispatcher

After GM_MENU exits, one dispatcher handles every terminal GM_* target. Targets are classified as:

- ported runtime: enter the real Vita bridge/state machine;
- menu-complete/runtime-bounded: prove the exact GM_* edge, prepare preload/state metadata as far as the current Vita runtime safely allows, then emit GAME_MENU_RUNTIME_FRONTIER;
- unsupported because retail data is missing: acceptable only when required game data is genuinely absent, with an explicit marker.

A correctly connected menu edge never silently changes to GM_MENU, GM_TITLE, or app exit because the target gameplay runtime is unfinished.

### 8. Back/return semantics are graph correctness

For every non-terminal node validate that Back returns to the retail parent, parent selection is preserved, prev_menu/cur_menu/hovered_selection/entering_menu/cooldown stay coherent, child GObjs/procs are destroyed exactly once, converted assets outlive their consumers, and re-entering the same child twice does not leave stale pointers or duplicate GX links.

### 9. Telemetry and graph coverage

Use structured markers:

- GAME_MENU_NODE_ENTER
- GAME_MENU_NODE_EXIT
- GAME_MENU_EDGE from=... selection=... to=...
- GAME_MENU_GM_EXIT mode=...
- GAME_MENU_RUNTIME_FRONTIER mode=...
- GAME_MENU_PROXY_SYMBOL
- GAME_MENU_DYNAMIC_CAPTURE_PASS
- FRAME_TIMING scene=...

Maintain a node/edge coverage bitmap and emit:

GAME_MENU_GRAPH_COVERAGE nodes=<visited>/<reachable> edges=<visited>/<reachable>

This makes one broad hardware sweep useful instead of requiring a new instrumented build after each menu bug.

## Implementation sequence for the single full-graph release

These are implementation tasks inside one release, not separate hardware milestones:

1. Freeze the reachable graph inventory above and add route IDs/log names.
2. Replace Title-specific exit scratch state with scene-correct menu route storage.
3. Normalize all internal MENU_KIND_* think callbacks and parent/back transitions.
4. Normalize all external panel enter/exit paths and remove Vita-only deferred/no-op actions.
5. Finish lazy MnMaAll symbol coverage for every reachable panel.
6. Finish dynamic JObj capture/order and classify the required SIS/SObj bridge work.
7. Convert GmEvent.dat and any other reachable big-endian data-only tables.
8. Add the generic terminal GM_* dispatcher and route every leaf to its exact retail mode.
9. Add graph coverage/edge telemetry and explicit runtime-frontier markers.
10. Build once the whole graph is connected; only then perform the physical-Vita navigation sweep.
11. Start the bug-fix pass from that single log set: crashes first, wrong return paths/input second, missing assets/text third, and visual fidelity after that.

## Definition of done for the menu graph

The menu-graph milestone is complete only when:

- every reachable selection produces the correct edge;
- no selectable entry is a no-op;
- no reachable submenu has a NULL think callback on Vita;
- no menu path depends on Title-only exit-data storage;
- every embedded panel can be entered, backed out of, and entered again;
- every terminal leaf reports the correct GM_* target;
- Classic/Adventure retain their existing CSS/first-match behavior;
- dynamic capture sees newly created menu JObjs;
- proxy misses and unsupported asset types are explicit, not silent;
- Event data is ARM-safe before Event code dereferences it;
- a full Vita build produces VELF, SELF and VPK;
- one hardware sweep can report graph coverage and all remaining failures can be handled as bug fixes without redesigning the graph.

## Bug-fix phase after graph completion

Once the full graph is wired, fixes are prioritized: crashes/branch-to-NULL/UAF and invalid big-endian pointers; wrong parent/selection/input routing; missing JObj/MatAnim/ShapeAnim/SIS/SObj content; GX order/TEV/blending/text fidelity; optional media such as Gallery MTHP; performance last.

Do not split those fixes back into isolated one-menu-per-release milestones unless a hardware-only fault makes it unavoidable.
