"""ARM regression for Corneria's generated diffuse+specular TEV graph."""
import argparse
import struct
from pathlib import Path

from arm_component import boot_component
from arm_harness import ArmHarness
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_CPSR,
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_R2,
)


p = argparse.ArgumentParser()
p.add_argument("--elf", required=True)
p.add_argument("--asset", required=True)
args = p.parse_args()

raw = Path(args.asset).read_bytes()
arm = ArmHarness(args.elf)


def cstring(text):
    raw_text = text.encode("ascii") + b"\0"
    ptr = arm.alloc(len(raw_text))
    arm.uc.mem_write(ptr, raw_text)
    return ptr


def u32(addr):
    return struct.unpack("<I", arm.uc.mem_read(addr, 4))[0]


def call_long(name, *call_args):
    try:
        return arm.call(name, *call_args)
    except AssertionError as exc:
        if str(exc) != f"{name}: did not return":
            raise
    for _ in range(16):
        pc = arm.uc.reg_read(UC_ARM_REG_PC)
        thumb = bool(arm.uc.reg_read(UC_ARM_REG_CPSR) & 32)
        arm.uc.emu_start(pc | thumb, arm.stop, timeout=30000000,
                         count=100000000)
        if arm.uc.reg_read(UC_ARM_REG_PC) == arm.stop:
            return 0
    raise AssertionError(f"{name}: timed out")


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
call_long("mv_stage_archive_prepare_raw", src, len(raw), cstring("GrCn.dat"))
archive = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
map_head = arm.call("HSD_ArchiveGetPublicAddress", archive, cstring("map_head"))
assert map_head
arm.call("mv_stage_archive_prepare", archive, map_head, 0, 0, 0)

records = u32(map_head + 8)
count = u32(map_head + 0x0C)
assert count == 11, count

arm.call("HSD_IDInitAllocData")
arm.call("HSD_IDSetup")
roots = []
for i in range(count):
    joint = u32(records + i * 0x34)
    if joint:
        root = arm.call("HSD_JObjLoadJoint", joint)
        assert root
        roots.append(root)

materials = []
seen_j = set()
seen_m = set()


def walk_jobj(jobj):
    while jobj:
        if jobj in seen_j:
            return
        seen_j.add(jobj)
        dobj = u32(jobj + 0x18)
        while dobj:
            mobj = u32(dobj + 8)
            if mobj and mobj not in seen_m:
                seen_m.add(mobj)
                materials.append((mobj, u32(mobj + 4), u32(mobj + 8)))
            dobj = u32(dobj + 4)
        child = u32(jobj + 0x10)
        if child:
            walk_jobj(child)
        jobj = u32(jobj + 8)


for root in roots:
    walk_jobj(root)

dual = 0
specular_add = 0
supported_specular = 0
for mobj, rendermode, tobj in materials:
    count_tobj = 0
    cur = tobj
    while cur:
        count_tobj += 1
        cur = u32(cur + 8)
    if count_tobj != 2:
        continue
    dual += 1

    arm.call("mv_gx_capture_reset_material_state")
    arm.call("HSD_MObjSetup", mobj, rendermode)
    state = arm.call("mv_gx_capture_material_state")
    assert state
    if arm.call("mv_gx_material_multitex_hsd_specular_add", state):
        specular_add += 1
        assert arm.call("mv_gx_material_uses_raster0", state) == 1
        prefix = bytes(arm.uc.mem_read(state, 160))
        # MvGxMaterialState: order-color starts at 74, color-in at 78.
        # Stage 2 must explicitly select COLOR1A1 and consume RASC.
        assert prefix[74 + 2] == 5, prefix[74:78]  # GX_COLOR1A1
        assert 10 in prefix[78 + 2 * 4:78 + 3 * 4]  # GX_CC_RASC
        if arm.call("mv_gx_material_multitex_vitagl_supported", state):
            supported_specular += 1

assert len(materials) == 275, len(materials)
assert dual == 22, dual
assert specular_add == 14, specular_add
assert supported_specular == 14, supported_specular

print(
    "PASS Corneria multitex ARM: 14/14 diffuse+specular materials use "
    "RASC0*TEX0 + RASC1*TEX1 and are accepted by the exact vitaGL path"
)
