#!/usr/bin/env python3
"""ARM regression for Vita FigaTrack filtering in fn_8001E60C."""

import struct

from arm_harness import ArmHarness
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0


arm = ArmHarness("build/vita-full/melee_vita")
allocations = []


def fake_fobj_alloc(machine, _address, _size, _data):
    ptr = arm.alloc(64)
    machine.mem_write(ptr, bytes(64))
    allocations.append(ptr)
    machine.reg_write(UC_ARM_REG_R0, ptr)
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


alloc_addr = arm.symbols["HSD_FObjAlloc"] & ~1
arm.uc.hook_add(UC_HOOK_CODE, fake_fobj_alloc, begin=alloc_addr, end=alloc_addr)


def make_tracks(obj_types):
    ptr = arm.alloc(len(obj_types) * 12)
    data = bytearray()
    for i, obj_type in enumerate(obj_types):
        # FigaTrack is 12 bytes on both PPC32 and ARM32.  The byte immediately
        # before ad_head is alignment padding and is not part of the payload.
        data += struct.pack(
            "<HHBBBBI",
            5 + (i & 1),
            0,
            obj_type,
            0x40 + i,
            0x80,
            0,
            0x1000 + i * 4,
        )
    arm.uc.mem_write(ptr, bytes(data))
    return ptr


# This matches the shape that crashed on hardware in Popo IntroR: the first
# three records are filtered rotation/scale channels, followed by six channels
# that must become FObjs.  A filtered record still consumes one FigaTrack.
allocations.clear()
mixed = make_tracks([7, 6, 5, 8, 9, 10, 1, 2, 3])
result = arm.call("fn_8001E60C", mixed, 9)
assert len(allocations) == 6, f"mixed: allocations={len(allocations)} expected=6"
assert result == allocations[0], f"mixed: result=0x{result:x} first=0x{allocations[0]:x}"

# Retail PAL contains thousands of nodes whose entire track group is filtered.
# Those groups must produce an empty FObj list without dereferencing garbage.
allocations.clear()
filtered = make_tracks([5, 6, 7])
result = arm.call("fn_8001E60C", filtered, 3)
assert allocations == [], f"filtered-only: unexpected allocations={len(allocations)}"
assert result == 0, f"filtered-only: result=0x{result:x} expected=NULL"

print("PASS lbAnim FigaTrack: filtered 5/6/7 tracks advance correctly and filtered-only groups return NULL")
