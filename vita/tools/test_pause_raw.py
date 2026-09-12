#!/usr/bin/env python3
"""Regression for GmPause SceneDesc HSD nativeization on the Vita ARM build."""

from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC


ASSET = Path("orig/GALE01/files/GmPause.dat")
PUBLIC = "ScGamPause_scene_data"
JOBJ_PTCL = 1 << 5
JOBJ_INSTANCE = 1 << 12


def parse_public(raw: bytes, wanted: str) -> int:
    if len(raw) < 32 or struct.unpack_from(">I", raw, 0)[0] != len(raw):
        raise AssertionError("bad GmPause DAT header")
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(">4I", raw, 4)
    reloc_off = 32 + data_size
    public_off = reloc_off + reloc_count * 4
    strings_off = public_off + (public_count + extern_count) * 8
    for i in range(public_count):
        off = public_off + i * 8
        target, name_off = struct.unpack_from(">2I", raw, off)
        start = strings_off + name_off
        end = raw.index(0, start)
        if raw[start:end].decode("ascii") == wanted:
            return target
    raise AssertionError(f"missing public {wanted}")


def be_ptr(raw: bytes, field: int) -> int:
    return struct.unpack_from(">I", raw, 32 + field)[0]


def walk_joint_offsets(raw: bytes, root: int):
    pending = [root]
    seen = set()
    while pending:
        off = pending.pop()
        if off in seen:
            continue
        seen.add(off)
        if off + 64 > struct.unpack_from(">I", raw, 4)[0]:
            raise AssertionError(f"joint outside DAT: 0x{off:x}")
        yield off
        child = be_ptr(raw, off + 8)
        next_ = be_ptr(raw, off + 12)
        if child:
            pending.append(child)
        if next_:
            pending.append(next_)


raw = ASSET.read_bytes()
scene = parse_public(raw, PUBLIC)
models = be_ptr(raw, scene)
model0 = be_ptr(raw, models)
joint0 = be_ptr(raw, model0)
assert joint0

false_union_flags = []
for off in walk_joint_offsets(raw, joint0):
    flags = struct.unpack_from(">I", raw, 32 + off + 4)[0]
    arm_unconverted = struct.unpack_from("<I", raw, 32 + off + 4)[0]
    assert not (flags & (JOBJ_PTCL | JOBJ_INSTANCE)), (
        f"GmPause contains a genuine unsupported union JObj at 0x{off:x}: 0x{flags:08x}"
    )
    if arm_unconverted & (JOBJ_PTCL | JOBJ_INSTANCE):
        false_union_flags.append((off, flags, arm_unconverted))

assert false_union_flags, "asset no longer reproduces a false PTCL/INSTANCE flag on ARM"

arm = ArmHarness("build/vita-full/melee_vita")


def silent(m, _address, _size, _data):
    m.reg_write(UC_ARM_REG_PC, m.reg_read(UC_ARM_REG_LR))


report = arm.symbols["OSReport"] & ~1
arm.uc.hook_add(UC_HOOK_CODE, silent, begin=report, end=report)
boot_component(arm)

src = arm.alloc(len(raw))
arm.uc.mem_write(src, raw)
# Match the production lbArchive_80016DBC("GmPause", ...) call exactly.  The
# nativeizer must recognize the archive from its public SceneDesc symbol rather
# than depending on the host/on-disc ".dat" suffix.
name = arm.alloc(len(b"GmPause\0"))
arm.uc.mem_write(name, b"GmPause\0")
arm.call("mv_boot_archive_prepare_raw", src, len(raw), name)

for off, flags, _ in false_union_flags:
    native = struct.unpack("<I", arm.uc.mem_read(src + 32 + off + 4, 4))[0]
    assert native == flags, (
        f"joint 0x{off:x} flags not nativeized: got 0x{native:08x}, expected 0x{flags:08x}"
    )

archive = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
scene_ptr = src + 32 + scene
models_ptr = struct.unpack("<I", arm.uc.mem_read(scene_ptr, 4))[0]
model0_ptr = struct.unpack("<I", arm.uc.mem_read(models_ptr, 4))[0]
joint0_ptr = struct.unpack("<I", arm.uc.mem_read(model0_ptr, 4))[0]
assert models_ptr and model0_ptr and joint0_ptr

root = arm.call("HSD_JObjLoadJoint", joint0_ptr)
assert root, "HSD_JObjLoadJoint returned NULL for GmPause model 0"
arm.call("HSD_JObjRemoveAll", root)

examples = ", ".join(
    f"0x{off:x}:0x{flags:08x}->0x{swapped:08x}"
    for off, flags, swapped in false_union_flags[:4]
)
print(
    f"PASS GmPause: {len(false_union_flags)} false ARM PTCL/INSTANCE flag(s) corrected; "
    f"HSD relocation/load/remove succeeded; examples {examples}"
)
