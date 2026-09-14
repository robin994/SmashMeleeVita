#!/usr/bin/env python3
"""ARM regression for Castle TEX0/TEX1 + generated TEV routing."""
from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R2

ROOT = Path(__file__).resolve().parents[2]
ASSET = ROOT / "orig/GALE01/files/GrCs.dat"
ELF = ROOT / "build/vita-full/melee_vita"

def cstring(arm, text):
    raw = text.encode("ascii") + b"\0"
    p = arm.alloc(len(raw))
    arm.uc.mem_write(p, raw)
    return p

def u32(arm, addr):
    return struct.unpack("<I", arm.uc.mem_read(addr, 4))[0]

def call_long(arm, name, *args):
    try:
        return arm.call(name, *args)
    except AssertionError as exc:
        if str(exc) != f"{name}: did not return":
            raise
    for _ in range(12):
        pc = arm.uc.reg_read(UD_ARM_REG_PC)
        thumb = bool(arm.uc.reg_read(UC_ARM_REG_CPSR) & 32)
        arm.uc.emu_start(pc | thumb, arm.stop, timeout=30000000, count=100000000)
        if arm.uc.reg_read(UC_ARM_REG_PC) == arm.stop:
            return 0
    raise AssertionError(f"{name}: timed out")

raw = ASSET.read_bytes()
arm = ArmHarness(str(ELF))

def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

def panic(machine, _address, _size, _data):
    msg = arm.string(machine.reg_read(UC_ARM_REG_R2)).decode(errors="replace")
    raise AssertionError("ARM HSD_Panic: " + msg)

for name, hook in (("OSReport", silent), ("HSD_Panic", panic)):
    addr = arm.symbols[name] & ~1
    arm.uc.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)
boot_component(arm)

src = arm.alloc(len(raw))
arm.uc.mem_write(src, raw)
call_long(arm, "mv_stage_archive_prepare_raw", src, len(raw), cstring(arm, "GrCs.dat"))
archive = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
map_head = arm.call("HSD_ArchiveGetPublicAddress", archive, cstring(arm, "map_head"))
assert map_head
arm.call("mv_stage_archive_prepare", archive, map_head, 0, 0, 0)
records = u32(arm, map_head + 8)
count = u32(arm, map_head + 0x0C)
assert count == 21, count

arm.call("HSD_IDInitAllocData")
arm.call("HSD_IDSetup")

roots = []
for i in range(count):
    joint = u32(arm, records + i * 0x34)
    if joint:
        root = arm.call("HSD_JObjLoadJoint", joint)
        assert root
        roots.append(root)

# Runtime layout: JObj next=+8 child=+0x10 dobj=+0x18;
# DObj next=+4 mobj=+8; MObj rendermode=+4 tobj=+8;
# TObj next=+8 src=+0x10.
materials = []
seen_j = set()
def walk_jobj(j):
    while j:
        if j in seen_j:
            return
        seen_j.add(j)
        dobj = u32(arm, j + 0x18)
        while dobj:
            mobj = u32(arm, dobj + 8)
            if mobj:
                t0 = u32(arm, mobj + 8)
                if t0:
                    t1 = u32(arm, t0 + 8)
                    if t1:
                        materials.append((mobj, u32(arm, mobj + 4), t0, t1,
                                          u32(arm, t0 + 0x10), u32(arm, t1 + 0x10)))
            dobj = u32(arm, dobj + 4)
        child = u32(arm, j + 0x10)
        if child:
            walk_jobj(child)
        j = u32(arm, j + 8)

for root in roots:
    walk_jobj(root)

tex01 = [m for m in materials if m[4] == 4 and m[5] == 5]
assert len(tex01) == 9, (len(materials), len(tex01))
mobj, rendermode, t0, t1, src0, src1 = tex01[0]
assert (rendermode & ~0x1000) == 0x32, hex(rendermode)

arm.call("mv_gx_capture_reset_material_state")
arm.call("HSD_MObjSetup", mobj, rendermode)
state = arm.call("mv_gx_capture_material_state")
assert state

# Prefix offsets are kept deliberately explicit: this test is also an ABI
# regression for the GX state consumed by gx_replay_vitagl.c.
prefix = bytes(arm.uc.mem_read(state, 160))
texture_count = prefix[59]
texgen_src0 = prefix[62]
texgen_src1 = prefix[63]
texgen_valid = prefix[64]
tev_count = prefix[65]
order_coord = list(prefix[66:70])
order_map = list(prefix[70:74])
order_color = list(prefix[74:78])
color_in = [list(prefix[78+i*4:82+i*4]) for i in range(4)]
alpha_in = [list(prefix[94+i*4:98+i*4]) for i in range(4)]
color_op = [list(prefix[110+i*5:115+i*5]) for i in range(4)]
alpha_op = [list(prefix[130+i*5:135+i*5]) for i in range(4)]

assert texture_count == 2, texture_count
assert texgen_valid & 3 == 3, hex(texgen_valid)
assert (texgen_src0, texgen_src1) == (4, 5), (texgen_src0, texgen_src1)
assert 1 <= tev_count <= 4, tev_count
assert arm.call("mv_gx_material_multitex_hsd_modulate", state) == 1
assert arm.call("mv_gx_material_multitex_vitagl_supported", state) == 1

# HSD_TObjSetupMtx loads post-texture matrices before GXSetTexCoordGen2.
# Verify both GX_PTTEXMTX0 (64) and GX_PTTEXMTX1 (67) survive the Vita GX
# state bridge, including Castle's non-identity second-layer atlas transform.
uv0 = arm.alloc(24)
uv1 = arm.alloc(24)
assert arm.call("mv_gx_capture_get_tex_mtx", 64, uv0) == 0
assert arm.call("mv_gx_capture_get_tex_mtx", 67, uv1) == 0
uv0_values = struct.unpack("<6f", arm.uc.mem_read(uv0, 24))
uv1_values = struct.unpack("<6f", arm.uc.mem_read(uv1, 24))
assert all(abs(v) < 1.0e6 for v in uv0_values + uv1_values)
assert any(abs(a - b) > 1.0e-5 for a, b in zip(uv1_values, (1, 0, 0, 0, 1, 0))), uv1_values

print("CASTLE_TEV",
      "count=", tev_count,
      "order_coord=", order_coord[:tev_count],
      "order_map=", order_map[:tev_count],
      "order_color=", order_color[:tev_count],
      "color_in=", color_in[:tev_count],
      "alpha_in=", alpha_in[:tev_count],
      "color_op=", color_op[:tev_count],
      "alpha_op=", alpha_op[:tev_count],
      flush=True)

# Static retail descriptors prove this is not a synthetic graph: all nine
# Castle dual-texture materials route the two texture objects from TEX0/TEX1.
assert len(tex01) == 9
print(
    "PASS Castle multitex ARM: 9 runtime materials use TEX0/TEX1; "
    f"HSD_MObjSetup generated {tev_count} captured TEV stages with routing 4/5 and dual post-texture matrices"
)

for root in roots:
    arm.call("HSD_JObjRemoveAll", root)
