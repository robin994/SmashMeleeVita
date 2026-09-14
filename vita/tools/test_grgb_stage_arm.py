#!/usr/bin/env python3
"""Regression for Great Bay stage roots through the original ARM HSD loader."""
from pathlib import Path
import struct

from arm_component import boot_component
from arm_harness import ArmHarness
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP

ROOT = Path(__file__).resolve().parents[2]
ELF = ROOT / "build/vita-full/melee_vita"
ASSET = ROOT / "orig/GALE01/files/GrGb.dat"
arm = ArmHarness(str(ELF))


def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


def panic(machine, _address, _size, _data):
    msg = arm.string(machine.reg_read(UC_ARM_REG_R2)).decode(errors="replace")
    regs = [machine.reg_read(r) for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_R7,UC_ARM_REG_R8,UC_ARM_REG_R9,UC_ARM_REG_R10,UC_ARM_REG_R11,UC_ARM_REG_R12,UC_ARM_REG_SP,UC_ARM_REG_LR)]
    print("PANIC regs=" + " ".join(f"r{i}={v:08x}" for i,v in enumerate(regs[:13])) + f" sp={regs[13]:08x} lr={regs[14]:08x}", flush=True)
    for v in regs:
        if 0x07400000 <= v < 0x07500000:
            try: print(f"PTR {v:08x}: " + machine.mem_read(v,32).hex(), flush=True)
            except Exception: pass
    try:
        dobj = regs[4]
        db = bytes(machine.mem_read(dobj, 24))
        mobj = int.from_bytes(db[8:12], "little")
        print(f"DOBJ {dobj:08x}: {db.hex()} mobj={mobj:08x}", flush=True)
        if mobj: print(f"MOBJ {mobj:08x}: " + machine.mem_read(mobj,32).hex(), flush=True)
        desc = regs[5]
        mb = int.from_bytes(machine.mem_read(desc+8,4), "little")
        print(f"DESC_MOBJ {mb:08x}: " + machine.mem_read(mb,24).hex(), flush=True)
    except Exception as e: print("DUMP_FAIL", e, flush=True)
    raise AssertionError("ARM HSD_Panic: " + msg)


for name, hook in (("OSReport", silent), ("HSD_Panic", panic)):
    addr = arm.symbols[name] & ~1
    arm.uc.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)
boot_component(arm)
raw = ASSET.read_bytes()
src = arm.alloc(len(raw))
arm.uc.mem_write(src, raw)
name = arm.alloc(len(ASSET.name) + 1)
arm.uc.mem_write(name, ASSET.name.encode() + b"\0")
arm.call("mv_stage_archive_prepare_raw", src, len(raw), name)
for off in (0x27D30, 0x27E84):
    value = struct.unpack("<I", arm.uc.mem_read(src + 32 + off + 4, 4))[0]
    assert value == 0x3C, (hex(off), hex(value))
archive = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
sym = arm.alloc(len("map_head") + 1)
arm.uc.mem_write(sym, b"map_head\0")
map_head = arm.call("HSD_ArchiveGetPublicAddress", archive, sym)
assert map_head
arm.call("mv_stage_archive_prepare", archive, map_head, 0, 0, 0)
maps = struct.unpack("<I", arm.uc.mem_read(map_head + 8, 4))[0]
count = struct.unpack("<I", arm.uc.mem_read(map_head + 12, 4))[0]
assert count == 10

# v3.74 regression: a retail shared-skin may reference a valid Joint whose
# descriptor has not been loaded into the HSD ID table yet. Force that exact
# state with a real Great Bay root and require PObj resolution to lazy-load it.
lazy_joint = struct.unpack("<I", arm.uc.mem_read(maps + 0 * 0x34, 4))[0]
assert lazy_joint
assert arm.call("mv_hsd_shared_skin_lazy_probe", lazy_joint) == 0
success = arm.alloc(4)
resolved = arm.call("HSD_IDGetDataFromTable", 0, lazy_joint, success)
assert resolved and struct.unpack("<I", arm.uc.mem_read(success, 4))[0] == 1
print(f"PASS GrGb shared-skin lazy ID resolve joint=0x{lazy_joint:08x} jobj=0x{resolved:08x}", flush=True)

for map_id in (0, 3, 4, 2, 1, 9):
    joint = struct.unpack("<I", arm.uc.mem_read(maps + map_id * 0x34, 4))[0]
    assert joint, map_id
    print(f"LOAD map={map_id} joint=0x{joint:08x}", flush=True)
    jobj = arm.call("HSD_JObjLoadJoint", joint)
    assert jobj, map_id
    arm.call("HSD_JObjRemoveAll", jobj)
print("PASS GrGb: native rendermodes and all retail Great Bay map roots load through original ARM HSD.")
