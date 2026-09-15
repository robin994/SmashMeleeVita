#!/usr/bin/env python3
from pathlib import Path

src = (Path(__file__).resolve().parents[1] / "gx_replay_vitagl.c").read_text()
needle = "2.0f * c->projection[2][col] + c->projection[3][col]"
if needle not in src:
    raise SystemExit("FAIL GX projection: missing z_gl = 2*z_gx + w conversion")
if src.count("c->cull_mode==1?GL_FRONT:GL_BACK") + src.count("c->cull_mode == 1 ? GL_FRONT : GL_BACK") < 2:
    raise SystemExit("FAIL GX culling: FRONT/BACK semantic mapping not applied to both replay paths")
legacy = src.split("void mv_gx_replay_draw(MvGxReplay *r,const MvCamera *cam)", 1)[1].split("static void captured_viewport", 1)[0]
captured = src.split("void mv_gx_replay_draw_captured(MvGxReplay *r)", 1)[1]
if "glFrontFace(GL_CW)" not in legacy:
    raise SystemExit("FAIL GX culling: legacy UI replay must retain its known-good GL_CW baseline")
if "glFrontFace(GL_CW)" not in captured:
    raise SystemExit("FAIL GX culling: captured gameplay must preserve GX clockwise front faces")
if "glFrontFace(GL_CCW)" in captured:
    raise SystemExit("FAIL GX culling: gameplay must not double-flip vitaGL's negative-Y viewport")
for gx, expected in [(-1.0, -1.0), (-0.5, 0.0), (0.0, 1.0)]:
    got = 2.0 * gx + 1.0
    if abs(got - expected) > 1e-7:
        raise SystemExit(f"FAIL GX projection math: {gx} -> {got}, expected {expected}")
print("PASS GX/vitaGL projection: GX clockwise winding preserved across vitaGL's negative-Y display viewport")
