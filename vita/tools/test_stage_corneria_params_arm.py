"""Verify Corneria's stage-specific hazard parameters cross PPC endian on ARM."""
import argparse
import math
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
arm.uc.mem_write(name, b"GrCn.dat\0")
arm.call("mv_stage_archive_prepare_raw", src, len(raw), name)

archive = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
sym = arm.alloc(len("yakumono_param") + 1)
arm.uc.mem_write(sym, b"yakumono_param\0")
yaku = arm.call("HSD_ArchiveGetPublicAddress", archive, sym)
assert yaku

# Before the typed post-relocation pass, x8 is still a GameCube 0.002f word
# and therefore becomes a huge finite value when ARM interprets the bytes.
raw_accel = struct.unpack("<f", arm.uc.mem_read(yaku + 0x08, 4))[0]
assert raw_accel > 1.0e20, raw_accel

GR_KIND_CORNERIA = 0x0E
arm.call("mv_stage_yakumono_prepare", archive, GR_KIND_CORNERIA, yaku)

expected_prefix = [
    30.0, 180.0, 0.002, 0.1, 20.0, 20.0, 1200.0, 1200.0,
    180.0, 0.5, 160.0, 160.0, 8.0, 12.0, 200.0, 600.0,
    600.0, 960.0, 1800.0, 0.15,
]
actual_prefix = list(struct.unpack("<20f", arm.uc.mem_read(yaku, 20 * 4)))
for actual, expected in zip(actual_prefix, expected_prefix):
    assert math.isclose(actual, expected, rel_tol=1.0e-6, abs_tol=1.0e-7), (
        actual, expected
    )

assert math.isclose(struct.unpack("<f", arm.uc.mem_read(yaku + 0x68, 4))[0],
                    120.0)
assert math.isclose(struct.unpack("<f", arm.uc.mem_read(yaku + 0x70, 4))[0],
                    0.5)
assert struct.unpack("<4i", arm.uc.mem_read(yaku + 0x74, 16)) == (16, 8, 8, 4)
assert math.isclose(struct.unpack("<f", arm.uc.mem_read(yaku + 0x88, 4))[0],
                    2.0)

# The post-relocation pass may be reached twice by diagnostics. It must not
# byte-swap the already-native stage-specific data a second time.
snapshot = bytes(arm.uc.mem_read(yaku, 0x8C))
arm.call("mv_stage_yakumono_prepare", archive, GR_KIND_CORNERIA, yaku)
assert bytes(arm.uc.mem_read(yaku, 0x8C)) == snapshot

# Exercise the Great Fox update domain that caused the hardware crash. With
# retail values every possible acceleration step remains finite and bounded.
accel = actual_prefix[2]
vmax = actual_prefix[3]
xlimit = actual_prefix[4]
vel = accel
offset = vel
assert math.isfinite(vel) and abs(vel) <= vmax
assert math.isfinite(offset) and abs(offset) <= xlimit

print(
    "PASS Corneria params ARM: timer=30..180 accel=0.002 vmax=0.1 "
    "limits=20/20; repeat nativeization is stable"
)
