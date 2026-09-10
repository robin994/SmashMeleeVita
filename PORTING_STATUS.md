# Porting status - 2026-09-10, v3.22 menu routing integration checkpoint

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
