#!/usr/bin/env python3
"""Full IfAll/HUD raw-schema and release-lifecycle regression on linked Vita ARM."""

from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC

ASSET = Path("orig/GALE01/files/IfAll.dat")
TABLES = (
    "DmgMrk_scene_models", "DmgNum_scene_models",
    "ScInfCnt_scene_models", "ScInfPnm_scene_models",
    "ScInfStc_scene_models", "ScInfTim_scene_models",
    "Stc_rarwmdls", "Stc_scemdls", "lupe", "tdsce",
)


def publics(raw: bytes):
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(">4I", raw, 4)
    public_off = 32 + data_size + reloc_count * 4
    strings_off = public_off + (public_count + extern_count) * 8
    out = {}
    for i in range(public_count):
        off = public_off + i * 8
        target, name_off = struct.unpack_from(">2I", raw, off)
        start = strings_off + name_off
        end = raw.index(0, start)
        out[raw[start:end].decode("ascii")] = target
    return out


raw = ASSET.read_bytes()
pub = publics(raw)
for name in TABLES + ("ScInfDmg_scene_data",):
    assert name in pub, name

arm = ArmHarness("build/vita-full/melee_vita")


def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


if "OSReport" in arm.symbols:
    p = arm.symbols["OSReport"] & ~1
    arm.uc.hook_add(UC_HOOK_CODE, silent, begin=p, end=p)
boot_component(arm)
heap = arm.call("HSD_GetHeap")

# Exact preload-type-2 boundary: generic dispatcher receives filename == NULL.
src = arm.alloc(len(raw))
arm.uc.mem_write(src, raw)
arm.call("mv_boot_archive_prepare_raw", src, len(raw), 0)
archive = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0

loaded = 0
unique_joints = set()
lupe_joint = 0
for table_name in TABLES:
    table = src + 32 + pub[table_name]
    for i in range(128):
        model = struct.unpack("<I", arm.uc.mem_read(table + i * 4, 4))[0]
        if model == 0:
            break
        joint = struct.unpack("<I", arm.uc.mem_read(model, 4))[0]
        if not joint:
            continue
        unique_joints.add(joint)
        root = arm.call("HSD_JObjLoadJoint", joint)
        assert root, (table_name, i, hex(joint))
        arm.call("HSD_JObjRemoveAll", root)
        loaded += 1
        if table_name == "lupe" and i == 0:
            lupe_joint = joint
    else:
        raise AssertionError(f"unterminated model table {table_name}")

assert lupe_joint
# Warm fixed-size pools, then prove the exact magnify model is lifecycle-stable.
for _ in range(2):
    root = arm.call("HSD_JObjLoadJoint", lupe_joint)
    arm.call("HSD_JObjRemoveAll", root)
baseline = arm.call("OSCheckHeap", heap)
series = []
for _ in range(32):
    root = arm.call("HSD_JObjLoadJoint", lupe_joint)
    assert root
    arm.call("HSD_JObjRemoveAll", root)
    series.append(arm.call("OSCheckHeap", heap))
assert min(series) == max(series) == baseline, (baseline, series)

print(
    f"PASS IfAll HUD: tables={len(TABLES)} model_loads={loaded} unique_joints={len(unique_joints)}; "
    f"preload filename=NULL nativeization + relocation/load/remove succeeded; "
    f"lupe heap stable for 32 cycles at {baseline} bytes free"
)
