#!/usr/bin/env python3
"""Verify effect parameter writes do not rely on original DOL BSS adjacency."""
import argparse
import struct

from arm_component import boot_component
from arm_harness import ArmHarness


p = argparse.ArgumentParser()
p.add_argument("--elf", required=True)
args = p.parse_args()

arm = ArmHarness(args.elf)
boot_component(arm)


def r16(addr):
    return struct.unpack("<H", arm.uc.mem_read(addr, 2))[0]


def r32(addr):
    return struct.unpack("<I", arm.uc.mem_read(addr, 4))[0]


particle_alloc = arm.symbols["hsd_804D0F60"]
anim_queue = arm.symbols["efLib_AnimQueue"]
param_table = arm.symbols["efLib_ParamTable"]

# This is the exact ARM/ELF layout that made the old matching expression
# efLib_AnimQueue + 0x10 entries alias the particle allocator instead of the
# independently linked parameter table.
assert param_table != anim_queue + 0x10 * 8
assert particle_alloc == anim_queue + 0x10 * 8

arm.call("hsd_80398A08", 0)
allocator_before = bytes(arm.uc.mem_read(particle_alloc, 0x30))

gobj = arm.alloc(0x20)
arm.uc.mem_write(gobj, bytes(0x20))
arm.call("efLib_SetParamGfxId", gobj, 0x417)
arm.call("efLib_SetParamAlpha", gobj, 0xFF)

allocator_after = bytes(arm.uc.mem_read(particle_alloc, 0x30))
assert allocator_after == allocator_before, (
    allocator_before.hex(),
    allocator_after.hex(),
)

assert r32(param_table) == gobj
assert r16(param_table + 4) == 0x417
assert r16(param_table + 6) == 0xFF

print(
    "PASS effect param ARM: ParamTable uses its real ELF symbol; "
    "gfx=0x417 alpha=0xff leave particle allocator untouched"
)
