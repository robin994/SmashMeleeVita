#!/usr/bin/env python3
"""Verify generator cleanup uses real particle globals on ARM/ELF."""
import argparse
import struct

from arm_component import boot_component
from arm_harness import ArmHarness

p = argparse.ArgumentParser()
p.add_argument("--elf", required=True)
args = p.parse_args()

arm = ArmHarness(args.elf)
boot_component(arm)

def r32(addr):
    return struct.unpack("<I", arm.uc.mem_read(addr, 4))[0]

def w32(addr, value):
    arm.uc.mem_write(addr, struct.pack("<I", value & 0xffffffff))

def w16(addr, value):
    arm.uc.mem_write(addr, struct.pack("<H", value & 0xffff))

particle_lists = arm.symbols["hsd_804D0908"]
particle_alloc = arm.symbols["hsd_804D0F60"]
particle_count = arm.symbols.get("hsd_804D78E2", 0)

# Retail DOL code historically treated the entire 0x4D08E8..0x4D0F60
# region as one struct. The ARM ELF is allowed to reorder these globals; the
# test intentionally never relies on their relative addresses.
assert particle_lists != particle_alloc

arm.call("hsd_80398A08", 0)
pp = arm.call("HSD_ObjAlloc", particle_alloc)
assert pp
arm.uc.mem_write(pp, bytes(0x98))

gen = arm.alloc(0x90)
arm.uc.mem_write(gen, bytes(0x90))
link = 3
ident = 0x1234
w16(gen + 0x1C, ident)
arm.uc.mem_write(gen + 0x19, bytes([link]))
w32(gen + 0x50, 1)

arm.uc.mem_write(pp + 0x1D, bytes([link]))
w16(pp + 0x1E, ident)
w32(pp + 0x88, gen)
w32(particle_lists + link * 4, pp)
if particle_count:
    w16(particle_count, 1)

used_before = r32(particle_alloc + 0x08)
assert used_before == 1, used_before
arm.call("hsd_8039D0A0", gen)

assert r32(particle_lists + link * 4) == 0
assert r32(gen + 0x50) == 0
assert r32(particle_alloc + 0x08) == 0
assert r32(particle_alloc + 0x0C) >= 1
assert r32(particle_alloc + 0x04) == pp

# Reuse proves that the free-list link itself remains valid after cleanup.
again = arm.call("HSD_ObjAlloc", particle_alloc)
assert again == pp, (hex(again), hex(pp))
print("PASS particle pool ARM: generator cleanup unlinks and reuses the real HSD particle allocator")
