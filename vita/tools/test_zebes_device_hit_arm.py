#!/usr/bin/env python3
"""Verify Zebes acid-hit metadata is native after HSD relocation on ARM."""
import argparse
import struct
from pathlib import Path

from arm_component import boot_component
from arm_harness import ArmHarness
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R2

p = argparse.ArgumentParser()
p.add_argument("--elf", required=True)
p.add_argument("--asset", required=True)
args = p.parse_args()

arm = ArmHarness(args.elf)

def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

def panic(machine, _address, _size, _data):
    msg = arm.string(machine.reg_read(UC_ARM_REG_R2)).decode(errors="replace")
    raise AssertionError("ARM HSD_Panic: " + msg)

for name, hook in (("OSReport", silent), ("HSD_Panic", panic)):
    addr = arm.symbols[name] & ~1
    arm.uc.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)

boot_component(arm)
raw = Path(args.asset).read_bytes()
src = arm.alloc(len(raw))
arm.uc.mem_write(src, raw)
name = arm.alloc(9)
arm.uc.mem_write(name, b"GrZe.dat\0")
arm.call("mv_stage_archive_prepare_raw", src, len(raw), name)

archive = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
sym = arm.alloc(len("yakumono_param") + 1)
arm.uc.mem_write(sym, b"yakumono_param\0")
yaku = arm.call("HSD_ArchiveGetPublicAddress", archive, sym)
assert yaku

GR_KIND_ZEBES = 8
arm.call("mv_stage_yakumono_prepare", archive, GR_KIND_ZEBES, yaku)
hit = struct.unpack("<I", arm.uc.mem_read(yaku + 0x2C, 4))[0]
assert hit
actual = list(struct.unpack("<9I", arm.uc.mem_read(hit, 9 * 4)))
expected = [1, 14, 90, 35, 0, 110, 1, 1, 8]
assert actual == expected, (actual, expected)

# The post-relocation pass must also be safe if a caller reaches it twice.
arm.call("mv_stage_yakumono_prepare", archive, GR_KIND_ZEBES, yaku)
again = list(struct.unpack("<9I", arm.uc.mem_read(hit, 9 * 4)))
assert again == expected, again
print("PASS Zebes device hit: acid damage=14 and all collision fields native on ARM")
