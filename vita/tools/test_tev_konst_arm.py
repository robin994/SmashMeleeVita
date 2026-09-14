#!/usr/bin/env python3
"""Verify generated one-stage TEV KONST state survives the ARM GX bridge."""
import argparse

from arm_component import boot_component
from arm_harness import ArmHarness

p = argparse.ArgumentParser()
p.add_argument("--elf", required=True)
args = p.parse_args()
arm = ArmHarness(args.elf)
boot_component(arm)
arm.call("mv_gx_capture_reset_material_state")

# GX stage: out.rgb = RASC*(1-KONST) + TEXC*KONST, out.a = RASA.
arm.call("GXSetNumTevStages", 1)
arm.call("GXSetTevOrder", 0, 0, 0, 4)
arm.call("GXSetTevColorIn", 0, 10, 8, 14, 15)
arm.call("GXSetTevAlphaIn", 0, 7, 7, 7, 5)
arm.call("GXSetTevColorOp", 0, 0, 0, 0, 1, 0)
arm.call("GXSetTevAlphaOp", 0, 0, 0, 0, 1, 0)

# Small GXColor structs are passed in one core register on ARM AAPCS. Bytes
# in memory are r,g,b,a = 40,80,c0,ff.
arm.call("GXSetTevKColor", 0, 0xFFC08040)
arm.call("GXSetTevKColorSel", 0, 0x14)  # K0_G
state = arm.call("mv_gx_capture_material_state")
assert arm.call("mv_gx_material_single_tev_rasc_tex_konst", state) == 1
assert arm.call("mv_gx_material_kcolor_rgba", state, 0) == 0x808080FF

arm.call("GXSetTevKColorSel", 0, 4)  # fixed 1/2
assert arm.call("mv_gx_material_kcolor_rgba", state, 0) == 0x7F7F7FFF
print("PASS TEV KONST ARM: generated RASC/TEXC interpolation captures K selector and register")
