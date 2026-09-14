#!/usr/bin/env python3
"""ARM regression for Castle TEX0/TEX1 + generated TEV routing."""
import argparse
from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R2

p = argparse.ArgumentParser()
p.add_argument("--elf", required=True)
p.add_argument("--asset", required=True)
args = p.parse_args()

ASSET = Path(args.asset)
ELF = Path(args.elf)

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
        pc = arm.uc.reg_read(UC_ARM_REG_PC)
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
# The exact retail texture must decode to visible pixels before vitaGL touches it.
image=u32(arm,state+0); palette=u32(arm,state+4); material_rgba=u32(arm,state+8)
width,height,pentries=struct.unpack("<HHH",arm.uc.mem_read(state+48,6))
fmt,pfmt,wraps,wrapt,mag,count=arm.uc.mem_read(state+54,6)
source_size=arm.call("mv_gx_texture_size",width,height,fmt)
out=arm.alloc(width*height*4)
assert arm.call("mv_gx_decode",out,width*height*4,width*4,image,source_size,width,height,fmt,palette,pentries*2,pfmt) == 0
pix=bytes(arm.uc.mem_read(out,width*height*4))
nonblack=sum(1 for i in range(0,len(pix),4) if pix[i] or pix[i+1] or pix[i+2])
nonzero_alpha=sum(1 for i in range(3,len(pix),4) if pix[i])
assert nonblack > (width*height)//16, (width,height,fmt,nonblack)
assert nonzero_alpha > (width*height)//16, (width,height,fmt,nonzero_alpha)
print("CASTLE_TEX0_PIXELS",width,height,fmt,hex(material_rgba),"nonblack",nonblack,"alpha",nonzero_alpha,flush=True)
# TEX1 participates multiplicatively too, so it must also contain visible samples.
image1=u32(arm,state+212); palette1=u32(arm,state+216)
width1,height1,pentries1=struct.unpack("<HHH",arm.uc.mem_read(state+228,6))
fmt1,pfmt1=arm.uc.mem_read(state+234,2)
source_size1=arm.call("mv_gx_texture_size",width1,height1,fmt1)
out1=arm.alloc(width1*height1*4)
decode1=arm.call("mv_gx_decode",out1,width1*height1*4,width1*4,image1,source_size1,width1,height1,fmt1,palette1,pentries1*2,pfmt1)
print("CASTLE_TEX1_META",hex(image1),hex(palette1),width1,height1,pentries1,fmt1,pfmt1,"size",source_size1,"decode",decode1,flush=True)
assert decode1 == 0
pix1=bytes(arm.uc.mem_read(out1,width1*height1*4))
nonblack1=sum(1 for i in range(0,len(pix1),4) if pix1[i] or pix1[i+1] or pix1[i+2])
nonzero_alpha1=sum(1 for i in range(3,len(pix1),4) if pix1[i])
assert nonblack1 > (width1*height1)//16, (width1,height1,fmt1,nonblack1)
assert nonzero_alpha1 > (width1*height1)//16, (width1,height1,fmt1,nonzero_alpha1)
print("CASTLE_TEX1_PIXELS",width1,height1,fmt1,"nonblack",nonblack1,"alpha",nonzero_alpha1,flush=True)
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

# Locate this material in a real HSD draw capture and prove CLR0 is visible too.
# These sizes/offsets are the Vita ARM ABI of gx_capture_vita.h.
CMD_SIZE=496; CMD_MATERIAL_OFF=172; VERTEX_SIZE=88; VERTEX_COLOR_OFF=64
found_color=False
for capture_root in roots:
    capture_stats=arm.alloc(64)
    if arm.call("mv_hsd_gx_capture_runtime",capture_root,1,0,capture_stats) != 0:
        continue
    ccount=arm.alloc(4); vcount=arm.alloc(4)
    cptr=arm.call("mv_gx_capture_commands",ccount)
    vptr=arm.call("mv_gx_capture_vertices",vcount)
    for cmd_index in range(u32(arm,ccount)):
        ca=cptr+cmd_index*CMD_SIZE
        first,vc,tris,attr=struct.unpack("<4I",arm.uc.mem_read(ca,16))
        cmd_image=u32(arm,ca+CMD_MATERIAL_OFF)
        if cmd_image != image:
            continue
        colors=[struct.unpack("<I",arm.uc.mem_read(vptr+(first+j)*VERTEX_SIZE+VERTEX_COLOR_OFF,4))[0] for j in range(vc)]
        visible_colors=[c for c in colors if (c >> 8) != 0 and (c & 0xff) != 0]
        assert visible_colors,(cmd_index,vc,[hex(c) for c in colors[:8]])
        print("CASTLE_CLR0_PIXELS","cmd",cmd_index,"verts",vc,"visible",len(visible_colors),"sample",hex(visible_colors[0]),flush=True)
        found_color=True
        break
    if found_color:
        break
assert found_color

# Static retail descriptors prove this is not a synthetic graph: all nine
# Castle dual-texture materials route the two texture objects from TEX0/TEX1.
assert len(tex01) == 9
print(
    "PASS Castle multitex ARM: 9 runtime materials use TEX0/TEX1; "
    f"HSD_MObjSetup generated {tev_count} captured TEV stages with routing 4/5 and dual post-texture matrices"
)

for root in roots:
    arm.call("HSD_JObjRemoveAll", root)
