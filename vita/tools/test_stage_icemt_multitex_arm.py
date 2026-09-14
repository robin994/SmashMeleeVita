#!/usr/bin/env python3
"""ARM regression for Icicle Mountain's dominant two-texture TEV graph."""
import argparse
from pathlib import Path
import struct

from arm_component import boot_component
from arm_harness import ArmHarness
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R2


p = argparse.ArgumentParser()
p.add_argument("--elf", required=True)
p.add_argument("--disc-root", required=True)
args = p.parse_args()

asset = Path(args.disc_root) / "GrIm.dat"
raw = asset.read_bytes()
arm = ArmHarness(args.elf)


def cstring(text):
    data = text.encode("ascii") + b"\0"
    ptr = arm.alloc(len(data))
    arm.uc.mem_write(ptr, data)
    return ptr


def u32(addr):
    return struct.unpack("<I", arm.uc.mem_read(addr, 4))[0]


def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


def panic(machine, _address, _size, _data):
    msg = arm.string(machine.reg_read(UC_ARM_REG_R2)).decode(errors="replace")
    raise AssertionError("ARM HSD_Panic: " + msg)


for name, hook in (("OSReport", silent), ("HSD_Panic", panic)):
    addr = arm.symbols[name] & ~1
    arm.uc.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)
boot_component(arm)


def call_long(name, *call_args):
    try:
        return arm.call(name, *call_args)
    except AssertionError as exc:
        if str(exc) != f"{name}: did not return":
            raise
    for _ in range(20):
        pc = arm.uc.reg_read(UC_ARM_REG_PC)
        thumb = bool(arm.uc.reg_read(UC_ARM_REG_CPSR) & 32)
        arm.uc.emu_start(pc | thumb, arm.stop, timeout=30000000, count=100000000)
        if arm.uc.reg_read(UC_ARM_REG_PC) == arm.stop:
            return 0
    raise AssertionError(f"{name}: timed out")


src = arm.alloc(len(raw))
arm.uc.mem_write(src, raw)
call_long("mv_stage_archive_prepare_raw", src, len(raw), cstring("GrIm.dat"))
archive = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
map_head = arm.call("HSD_ArchiveGetPublicAddress", archive, cstring("map_head"))
assert map_head
arm.call("mv_stage_archive_prepare", archive, map_head, 0, 0, 0)

records = u32(map_head + 8)
record_count = u32(map_head + 0x0C)
assert record_count == 9, record_count

arm.call("HSD_IDInitAllocData")
arm.call("HSD_IDSetup")

roots = []
for i in range(record_count):
    joint = u32(records + i * 0x34)
    if joint:
        root = arm.call("HSD_JObjLoadJoint", joint)
        assert root
        roots.append(root)

materials = []
seen_jobj = set()
seen_mobj = set()


def walk_jobj(jobj):
    while jobj:
        if jobj in seen_jobj:
            return
        seen_jobj.add(jobj)
        dobj = u32(jobj + 0x18)
        while dobj:
            mobj = u32(dobj + 8)
            if mobj and mobj not in seen_mobj:
                seen_mobj.add(mobj)
                materials.append((mobj, u32(mobj + 4), u32(mobj + 8)))
            dobj = u32(dobj + 4)
        child = u32(jobj + 0x10)
        if child:
            walk_jobj(child)
        jobj = u32(jobj + 8)


for root in roots:
    walk_jobj(root)

two_texture = 0
mode_rasa = 0
mode_tex0_alpha = 0
vitagl_supported = 0

for mobj, rendermode, tobj in materials:
    if not tobj or not u32(tobj + 8):
        continue
    two_texture += 1
    arm.call("mv_gx_capture_reset_material_state")
    arm.call("HSD_MObjSetup", mobj, rendermode)
    state = arm.call("mv_gx_capture_material_state")
    assert state
    mode = arm.call("mv_gx_material_multitex_hsd_alpha_blend", state)
    if mode == 1:
        mode_rasa += 1
    elif mode == 2:
        mode_tex0_alpha += 1
    if arm.call("mv_gx_material_multitex_vitagl_supported", state):
        vitagl_supported += 1

assert len(materials) == 756, len(materials)
assert two_texture == 207, two_texture
assert mode_rasa == 205, mode_rasa
assert mode_tex0_alpha == 2, mode_tex0_alpha
assert vitagl_supported == 207, vitagl_supported

print(
    "PASS Icicle multitex ARM: 207/207 two-texture materials supported; "
    "205 use RASC*TEX0 then alpha-blend TEX1, 2 preserve TEX0 alpha"
)

for root in roots:
    arm.call("HSD_JObjRemoveAll", root)
