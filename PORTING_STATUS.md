# Porting status — 2026-09-09, v2.5 GX PE blend/culling milestone

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
- VPK/asset ZIP integrity checks passed. VPK contains only eboot.bin and sce_sys/param.sfo.

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

## Next implementation work

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

## Next real-Vita check

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

## Current artifacts

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
