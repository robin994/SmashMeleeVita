# SmashMeleeVita

Experimental native PS Vita port based on [doldecomp/melee](https://github.com/doldecomp/melee).
**Not playable yet.** v3.67 runs the original opening/title/menu callbacks, Character Select and
Stage Select, then continues through Melee's original `GameMode` and `GameScene` loop toward the
first real Training/VS match. GameCube HSD/GX output is converted on ARM and replayed by vitaGL.
Upstream history and source provenance are preserved. Port changes are currently local.

The v3.67 package removes the confirmed `MnSlMap.usd` ShapeSet crash and repairs the common SIS
camera/projection path used by menu labels, CSS names and difficulty text. Linked-ARM regressions
pass; the visual fixes and first match still require validation on physical Vita hardware.

See [PORTING_STATUS.md](PORTING_STATUS.md) for verified results and remaining work.
Original GameCube build instructions remain in [.github/README.md](.github/README.md).

## Extract the user's original disc

```sh
python3 vita/tools/extract_disc.py "orig/GALE01/Super Smash Bros. Melee (USA) (En,Ja) (v1.02).iso"
```

Requires USA 1.02 / GALE01 revision 2. The extractor verifies `main.dol` against upstream SHA-1
`08e0bf20134dfcb260699671004527b2d6bb1a45` before writing output. It preserves the ISO and refuses
to replace differing existing files. Re-running verifies existing bytes.
Output: `orig/GALE01/sys/main.dol`, `orig/GALE01/files/`, and a SHA-256 extraction manifest.
ISO, extracted files, manifest and build artifacts are ignored by Git.

## Build and install the current Vita test

Requirements: VitaSDK with vita2d, CMake, Make, and Python 3.

```sh
make -f Makefile.vita
make -f Makefile.vita full-assets
```

1. Install the current named hardware candidate
   `build/vita-full/SmashMeleeVita-v3.67-original-gamemode-sss-shapeset-sis.vpk` with VitaShell
   (title ID `SMEL00001`, APP_VER `00.77`). A default local rebuild writes the same package as
   `build/vita/SmashMeleeVita-assets.vpk`.
2. For the retail runtime, copy the **complete** extracted `orig/GALE01/files/`
   tree recursively to `ux0:data/SmashMeleeVita/files/`. In particular, keep
   subdirectories such as `audio/` and `audio/us/`; copying only the old
   menu/boot ZIP will deadlock/fail when the original game requests SFX banks.
   `make -f Makefile.vita full-assets` creates a hash-verified local ZIP with
   the same complete tree if transferring a single archive is more convenient.
3. Launch Melee Vita Runtime. It runs the opening movie, original title and original main menu.
   For the current first-match test, choose 1P -> Training, select a character, press Start,
   select a stage and press Start. SELECT+START remains the emergency exit chord.
4. Retrieve `ux0:data/SmashMeleeVita/runtime.log` after the run. Each launch replaces the log.

The VPK contains program code only. The local data ZIP contains a hash-verified copy of the
user's original disc files, not preconverted images. Data decoding takes place on Vita at runtime.
A missing/corrupt archive produces an error screen and a log marker.

The PAD diagnostics map Cross -> A, Square -> B, Circle -> X, Triangle -> Y, Select -> Z,
Start -> Start, left stick -> main stick, right stick -> C-stick, D-pad -> D-pad, L/R -> full
trigger pressure. Analog trigger pressure, rumble and additional controllers are not implemented.

## Verify locally

```sh
make -f Makefile.vita asset-check
python3 -m venv build/vita/test-env
build/vita/test-env/bin/python -m pip install -r vita/tools/requirements-test.txt
build/vita/test-env/bin/python vita/tools/test_arm.py --assets orig/GALE01/files/MnMaAll.usd
```

`asset-check` builds the same data/texture sources on the host with AddressSanitizer and
UndefinedBehaviorSanitizer. It tests format vectors, malformed data and extraction preservation;
validates archive containers; decodes the menu textures into `build/vita/host/menu-textures/`.
It writes `archive-audit.json` and `menu-textures/report.json` under `build/vita/host/`.

The ARM harness executes the actual linked ELF with modeled libc memory/string operations.
It checks RNG/PAD, static geometry, the original menu camera and visibility classification,
native HSD descriptors, all menu texture descriptors and one real texture for each format present
against the host result. It also executes original HSD component initialization and the port heap/VI state code,
with modeled libc services and vblank waits. It does not validate the Vita GPU or physical controller.
Real PS Vita testing is still required.

The earlier whole-tree compiler inventory is available with `make -f Makefile.vita audit`.
It compiles individual files without linking the game and includes original SDK/runtime units
that need replacement; its success count is not a completion percentage.
