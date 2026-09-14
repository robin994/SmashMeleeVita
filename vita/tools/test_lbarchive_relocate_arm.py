#!/usr/bin/env python3
"""ARM regression for shared fighter mini-DAT relocation on Vita."""
from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC


def make_dat():
    data = struct.pack(">II", 4, 0x11223344)
    reloc = struct.pack(">I", 0)
    public = struct.pack(">II", 0, 0)
    strings = b"root\0"
    total = 0x20 + len(data) + len(reloc) + len(public) + len(strings)
    header = struct.pack(">5I4s2I", total, len(data), 1, 1, 0,
                         b"0011", 0, 0)
    return header + data + reloc + public + strings


arm = ArmHarness("build/vita-full/melee_vita")


def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


report = arm.symbols["OSReport"] & ~1
arm.uc.hook_add(UC_HOOK_CODE, silent, begin=report, end=report)
boot_component(arm)

raw = make_dat()
src = arm.alloc(len(raw))
dst = arm.alloc(len(raw))
arm.uc.mem_write(src, raw)

archive_src = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive_src, src, len(raw)) == 0

# HSD_ArchiveParse leaves the serialized header/metadata big-endian but turns
# relocation-backed data words into live ARM pointers.  This is exactly the
# source state copied from Popo to Nana by ftData_80085CD8/ftData_80085E50.
src_pointer = struct.unpack("<I", arm.uc.mem_read(src + 0x20, 4))[0]
assert src_pointer == src + 0x24

arm.uc.mem_write(dst, bytes(arm.uc.mem_read(src, len(raw))))
archive_dst = arm.alloc(0x44)
delta = (dst - src) & 0xFFFFFFFF
assert arm.call("lbArchiveRelocate", archive_dst, dst, len(raw), delta) == 0

dst_pointer = struct.unpack("<I", arm.uc.mem_read(dst + 0x20, 4))[0]
assert dst_pointer == dst + 0x24, (
    f"rebased pointer 0x{dst_pointer:08x} != 0x{dst + 0x24:08x}"
)

name = arm.alloc(5)
arm.uc.mem_write(name, b"root\0")
root = arm.call("HSD_ArchiveGetPublicAddress", archive_dst, name)
assert root == dst + 0x20

print("PASS lbArchiveRelocate Vita: big-endian header/reloc metadata + already-relocated shared mini-DAT pointers rebase correctly")
