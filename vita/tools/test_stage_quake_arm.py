#!/usr/bin/env python3
"""ARM regression for stage quake DynamicModelDesc nativeization."""
from pathlib import Path
import math
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC

ASSET = Path("orig/GALE01/files/GrCs.dat")

def parse_dat(raw: bytes):
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(">4I", raw, 4)
    reloc_base = 32 + data_size
    public_base = reloc_base + reloc_count * 4
    strings_base = public_base + (public_count + extern_count) * 8
    relocs = {struct.unpack_from(">I", raw, reloc_base + i * 4)[0] for i in range(reloc_count)}
    publics = {}
    for i in range(public_count):
        target, name_off = struct.unpack_from(">2I", raw, public_base + i * 8)
        start = strings_base + name_off
        end = raw.index(0, start)
        publics[raw[start:end].decode("ascii")] = target
    return data_size, relocs, publics

raw = ASSET.read_bytes()
data_size, relocs, publics = parse_dat(raw)
assert "quake_model_set" in publics
data = raw[32:32 + data_size]
model = publics["quake_model_set"]
assert model + 0 in relocs and model + 4 in relocs
joint = struct.unpack_from(">I", data, model)[0]
anim_table = struct.unpack_from(">I", data, model + 4)[0]
assert joint and anim_table

anim_roots = []
for i in range(8):
    field = anim_table + i * 4
    if field not in relocs:
        break
    anim_roots.append(struct.unpack_from(">I", data, field)[0])
assert len(anim_roots) == 4, [hex(x) for x in anim_roots]

expected = []
for root in anim_roots:
    child, next_, aobj, robj, flags = struct.unpack_from(">5I", data, root)
    assert child == 0 and next_ == 0 and robj == 0 and aobj
    a_flags = struct.unpack_from(">I", data, aobj)[0]
    end_frame = struct.unpack_from(">f", data, aobj + 4)[0]
    fobj = struct.unpack_from(">I", data, aobj + 8)[0]
    assert math.isfinite(end_frame) and end_frame > 0 and fobj
    fobjs = []
    while fobj:
        next_fobj = struct.unpack_from(">I", data, fobj)[0]
        length = struct.unpack_from(">I", data, fobj + 4)[0]
        start = struct.unpack_from(">f", data, fobj + 8)[0]
        obj_type = data[fobj + 12]
        assert length > 0 and math.isfinite(start) and obj_type in (5, 6)
        fobjs.append((fobj, length, start, obj_type))
        fobj = next_fobj
    assert len(fobjs) == 2
    expected.append((root, flags, aobj, a_flags, end_frame, fobjs))

pointer_bytes = {}
for off in (model, model + 4):
    pointer_bytes[off] = data[off:off + 4]
for i in range(4):
    off = anim_table + i * 4
    pointer_bytes[off] = data[off:off + 4]

arm = ArmHarness("build/vita-full/melee_vita")
def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))
if "OSReport" in arm.symbols:
    addr = arm.symbols["OSReport"] & ~1
    arm.uc.hook_add(UC_HOOK_CODE, silent, begin=addr, end=addr)
boot_component(arm)
src = arm.alloc(len(raw))
arm.uc.mem_write(src, raw)
name = arm.alloc(len(b"GrCs.dat\0"))
arm.uc.mem_write(name, b"GrCs.dat\0")
arm.call("mv_stage_archive_prepare_raw", src, len(raw), name)
native = src + 32

for off, original in pointer_bytes.items():
    assert bytes(arm.uc.mem_read(native + off, 4)) == original, hex(off)

for root, flags, aobj, a_flags, end_frame, fobjs in expected:
    got_flags = struct.unpack("<I", arm.uc.mem_read(native + root + 16, 4))[0]
    assert got_flags == flags, (hex(root), hex(got_flags), hex(flags))
    got_a_flags = struct.unpack("<I", arm.uc.mem_read(native + aobj, 4))[0]
    got_end = struct.unpack("<f", arm.uc.mem_read(native + aobj + 4, 4))[0]
    assert got_a_flags == a_flags, (hex(aobj), hex(got_a_flags), hex(a_flags))
    assert math.isclose(got_end, end_frame, rel_tol=1e-6, abs_tol=1e-6), (hex(aobj), got_end, end_frame)
    for fobj, length, start, obj_type in fobjs:
        got_len = struct.unpack("<I", arm.uc.mem_read(native + fobj + 4, 4))[0]
        got_start = struct.unpack("<f", arm.uc.mem_read(native + fobj + 8, 4))[0]
        got_type = arm.uc.mem_read(native + fobj + 12, 1)[0]
        assert got_len == length, (hex(fobj), got_len, length)
        assert math.isclose(got_start, start, rel_tol=1e-6, abs_tol=1e-6), (hex(fobj), got_start, start)
        assert got_type == obj_type

first_fobj = expected[0][5][0]
assert first_fobj[1] == 5
raw_le_length = struct.unpack("<I", struct.pack(">I", first_fobj[1]))[0]
assert raw_le_length == 0x05000000

# Run the real quake JObj animations through the linked ARM HSD runtime, not
# merely the descriptor converter. This proves that the FObj streams produce
# finite translation values before Camera_ApplyQuake consumes them.
archive = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
runtime_joint = struct.unpack("<I", arm.uc.mem_read(native + model, 4))[0]
runtime_anim_table = struct.unpack("<I", arm.uc.mem_read(native + model + 4, 4))[0]
assert runtime_joint and runtime_anim_table
animated_samples = 0
for index, (_root, _flags, _aobj, _a_flags, end_frame, _fobjs) in enumerate(expected):
    anim = struct.unpack("<I", arm.uc.mem_read(runtime_anim_table + index * 4, 4))[0]
    assert anim
    jobj = arm.call("HSD_JObjLoadJoint", runtime_joint)
    assert jobj
    arm.call("HSD_JObjAddAnimAll", jobj, anim, 0, 0)
    arm.call("HSD_JObjReqAnimAll", jobj, 0)
    for _frame in range(int(end_frame) + 2):
        arm.call("HSD_JObjAnimAll", jobj)
        xyz = struct.unpack("<3f", arm.uc.mem_read(jobj + 0x38, 12))
        assert all(math.isfinite(v) and abs(v) < 10000.0 for v in xyz), (index, _frame, xyz)
        animated_samples += 1
    arm.call("HSD_JObjRemoveAll", jobj)

print(f"PASS stage quake endian/runtime: GrCs joint + 4 AnimJoint roots + 8 FObj descriptors nativeized; {animated_samples} ARM animation samples finite; length=5 no longer aliases 0x05000000")
