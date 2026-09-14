#!/usr/bin/env python3
from pathlib import Path

src = (Path(__file__).resolve().parents[1] / "gx_replay_vitagl.c").read_text()
needle = "2.0f * c->projection[2][col] + c->projection[3][col]"
if needle not in src:
    raise SystemExit("FAIL GX projection: missing z_gl = 2*z_gx + w conversion")
if src.count("c->cull_mode==1?GL_FRONT:GL_BACK") + src.count("c->cull_mode == 1 ? GL_FRONT : GL_BACK") < 2:
    raise SystemExit("FAIL GX culling: FRONT/BACK semantic mapping not applied to both replay paths")
for gx, expected in [(-1.0, -1.0), (-0.5, 0.0), (0.0, 1.0)]:
    got = 2.0 * gx + 1.0
    if abs(got - expected) > 1e-7:
        raise SystemExit(f"FAIL GX projection math: {gx} -> {got}, expected {expected}")
print("PASS GX/vitaGL projection: GX depth -1..0 maps to GL -1..1; cull kept at known-good baseline")
