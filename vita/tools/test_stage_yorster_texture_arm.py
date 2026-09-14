#!/usr/bin/env python3
"""ARM regression for Yoshi's Island single-texture TEV and alpha compare."""
import argparse
import struct
from pathlib import Path

from arm_component import boot_component
from arm_harness import ArmHarness
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R2

p = argparse.ArgumentParser()
p.add_argument("--elf", required=True)
p.add_argument("--asset", required=True)
args = p.parse_args()
raw = Path(args.asset).read_bytes()
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
call_long("mv_stage_archive_prepare_raw", src, len(raw), cstring("GrYt.dat"))
archive = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
map_head = arm.call("HSD_ArchiveGetPublicAddress", archive, cstring("map_head"))
assert map_head
arm.call("mv_stage_archive_prepare", archive, map_head, 0, 0, 0)
records = u32(map_head + 8)
record_count = u32(map_head + 0x0C)
assert record_count == 2, record_count

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
                materials.append((mobj, u32(mobj + 4)))
            dobj = u32(dobj + 4)
        child = u32(jobj + 0x10)
        if child:
            walk_jobj(child)
        jobj = u32(jobj + 8)


for root in roots:
    walk_jobj(root)

alpha_texa = 0
alpha_rasa = 0
for mobj, rendermode in materials:
    arm.call("mv_gx_capture_reset_material_state")
    arm.call("HSD_MObjSetup", mobj, rendermode)
    state = arm.call("mv_gx_capture_material_state")
    mode = arm.call("mv_gx_material_single_tev_rasc_tex", state)
    if mode == 1:
        alpha_texa += 1
    elif mode == 2:
        alpha_rasa += 1

assert len(materials) == 138, len(materials)
assert alpha_texa == 72, alpha_texa
assert alpha_rasa == 46, alpha_rasa

# The runtime stage draw uses 301 non-trivial GX alpha comparisons.  Every one
# must now be reducible exactly to vitaGL's fixed-function alpha test instead
# of being dropped before replay.
root = roots[-1]
stats = arm.alloc(64)
assert arm.call("mv_hsd_gx_capture_runtime", root, 1, 0, stats) == 0
count_ptr = arm.alloc(4)
commands = arm.call("mv_gx_capture_commands", count_ptr)
count = u32(count_ptr)
CMD_SIZE = 496
MATERIAL_OFF = 172
nontrivial = 0
rejected = 0
patterns = {}
for i in range(count):
    material = commands + i * CMD_SIZE + MATERIAL_OFF
    comp0, ref0, op, comp1, ref1 = arm.uc.mem_read(material + 0x1C, 5)
    if comp0 == 7 and comp1 == 7:  # GX_ALWAYS/GX_ALWAYS
        continue
    nontrivial += 1
    key = (comp0, ref0, op, comp1, ref1)
    patterns[key] = patterns.get(key, 0) + 1
    if not arm.call("mv_gx_alpha_compare_vitagl_supported", comp0, ref0, op, comp1, ref1):
        rejected += 1

assert nontrivial == 301, nontrivial
assert rejected == 0, (rejected, patterns)
assert patterns.get((6, 0, 0, 6, 0)) == 284, patterns
assert sum(v for k, v in patterns.items() if k[0] == 6 and k[2] == 0 and k[3] == 3 and k[4] == 255) == 17, patterns

print(
    "PASS Yoshi Island texture ARM: 118 exact RASC*TEX0 materials "
    "(72 RASA*TEXA, 46 RASA) and all 301 runtime GX alpha compares are replayable"
)
