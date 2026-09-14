#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
replay = (root / "gx_replay_vitagl.c").read_text()
capture = (root / "gx_capture_vita.c").read_text()

required_replay = [
    "mv_gx_material_multitex_hsd_modulate(m) ||",
    "mv_gx_material_single_tev_rasc_tex_konst(m)) return 0;",
    "src == GX_TG_TEX1) return v->tex1",
    "GL_SRC0_RGB, GL_PRIMARY_COLOR",
    "GL_SRC1_RGB, GL_TEXTURE",
    "GL_SRC0_ALPHA, GL_PRIMARY_COLOR",
    "setup_texture1_env(m, hsd_two, hsd_alpha_blend)",
]
for needle in required_replay:
    if needle not in replay:
        raise SystemExit(f"FAIL vitaGL Castle multitex: missing {needle}")
required_capture = [
    "attr == GX_VA_TEX0 || attr == GX_VA_TEX1",
    "attr == GX_VA_TEX0 ? out->tex0 : out->tex1",
    "out->present |= 1u << attr_index",
    "tex_mtx_id[slot] = id",
]
for needle in required_capture:
    if needle not in capture:
        raise SystemExit(f"FAIL GX Castle capture: missing {needle}")
print("PASS Castle vitaGL bridge: CLR0/TEX0/TEX1 presence, raw TEX0, distinct TEX1 routing and fixed-function two-stage TEV are wired")
