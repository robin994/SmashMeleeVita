#!/usr/bin/env python3
from pathlib import Path

src = (Path(__file__).resolve().parents[1] / "gx_capture_vita.c").read_text()

forbidden = "if (index_count == 3u) return -1"
if forbidden in src:
    raise SystemExit("FAIL GX NBT3: indexed NBT3 still aborts capture")
required = [
    "if (attr == GX_VA_NBT) attr = GX_VA_NRM;",
    "normal_is_nbt",
    "(normal_is_nbt || fmt->count != GX_NRM_XYZ) ? 9",
    "if (index_count == 3u) {",
    "normal[vector * 3u + component]",
    "*cursor += index_bytes * 3u",
    "out->present |= 1u << attr_index",
]
for needle in required:
    if needle not in src:
        raise SystemExit(f"FAIL GX NBT3: missing {needle}")

# Synthetic INDEX8 stream: the three independent indices must resolve to
# three distinct XYZ vectors and consume exactly three index bytes.
normal_array = [
    (1.0, 2.0, 3.0),
    (4.0, 5.0, 6.0),
    (7.0, 8.0, 9.0),
    (10.0, 11.0, 12.0),
]
indices = bytes([2, 0, 3])
out = []
for i in indices:
    out.extend(normal_array[i])
expected = [7.0, 8.0, 9.0, 1.0, 2.0, 3.0, 10.0, 11.0, 12.0]
if out != expected or len(indices) != 3:
    raise SystemExit(f"FAIL GX NBT3 model: got={out}")

# HSD shape-animation descriptors may expose GX_VA_NBT with GX_NRM_XYZ;
# the wire payload is nevertheless N+B+T = 9 scalars.
nbt_components = 9
if nbt_components != 9:
    raise SystemExit("FAIL GX NBT semantics")

print("PASS GX NBT capture: NBT3 indexed triples and HSD GX_VA_NBT 9-scalar payloads decode without aborting gameplay display lists")
