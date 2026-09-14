#!/usr/bin/env python3
"""ARM regression for the Training GmTrain DynamicModelDesc HSD graph."""

from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC

ASSETS = Path("orig/GALE01/files")
CASES = ("GmTrain.dat", "GmTrain.usd")
ROOT = "ScGamTraining_scene_models"
JOBJ_PTCL = 0x20


def parse_raw(raw: bytes):
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(">4I", raw, 4)
    data_base = 32
    reloc_base = data_base + data_size
    public_base = reloc_base + reloc_count * 4
    strings_base = public_base + (public_count + extern_count) * 8
    reloc = {
        struct.unpack_from(">I", raw, reloc_base + i * 4)[0]
        for i in range(reloc_count)
    }
    publics = {}
    for i in range(public_count):
        target, name_off = struct.unpack_from(">2I", raw, public_base + i * 8)
        start = strings_base + name_off
        end = raw.index(0, start)
        publics[raw[start:end].decode("ascii")] = target
    return data_base, reloc, publics


def bswap32(v):
    return struct.unpack("<I", struct.pack(">I", v))[0]


def retail_joint_graph(raw: bytes, data_base: int, reloc: set[int], root: int):
    def be32(off):
        return struct.unpack_from(">I", raw, data_base + off)[0]

    def ptr(off):
        return be32(off) if off in reloc else 0

    model = ptr(root)
    assert model
    joint_root = ptr(model)
    assert joint_root
    seen = set()
    flags = {}

    def walk(joint):
        if not joint or joint in seen:
            return
        seen.add(joint)
        flags[joint] = be32(joint + 4)
        walk(ptr(joint + 8))
        walk(ptr(joint + 12))

    walk(joint_root)
    return joint_root, flags


arm = ArmHarness("build/vita-full/melee_vita")


def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


if "OSReport" in arm.symbols:
    p = arm.symbols["OSReport"] & ~1
    arm.uc.hook_add(UC_HOOK_CODE, silent, begin=p, end=p)
boot_component(arm)


def cstring(text: str):
    raw = text.encode("ascii") + b"\0"
    ptr = arm.alloc(len(raw))
    arm.uc.mem_write(ptr, raw)
    return ptr


def u32(addr):
    return struct.unpack("<I", arm.uc.mem_read(addr, 4))[0]


total_false_ptcl = 0
for filename in CASES:
    raw = (ASSETS / filename).read_bytes()
    data_base, reloc, publics = parse_raw(raw)
    assert ROOT in publics, (filename, ROOT)
    table = publics[ROOT]
    _joint_root, expected_flags = retail_joint_graph(raw, data_base, reloc, table)
    false_ptcl = sum(
        1 for value in expected_flags.values()
        if not (value & JOBJ_PTCL) and (bswap32(value) & JOBJ_PTCL)
    )
    assert false_ptcl == 10, (filename, false_ptcl)
    assert all(not (value & JOBJ_PTCL) for value in expected_flags.values()), filename
    total_false_ptcl += false_ptcl

    src = arm.alloc(len(raw))
    arm.uc.mem_write(src, raw)
    arm.call("mv_boot_archive_prepare_raw", src, len(raw), cstring(filename))

    # Every serialized JObj flag must now be host-native. This is the exact
    # boundary that was missing on v3.82 and made ROOT_XLU 0x20000000 look like
    # JOBJ_PTCL 0x00000020 on little-endian ARM.
    for offset, expected in expected_flags.items():
        actual = u32(src + data_base + offset + 4)
        assert actual == expected, (filename, hex(offset), hex(actual), hex(expected))
        assert not (actual & JOBJ_PTCL), (filename, hex(offset), hex(actual))

    archive = arm.alloc(0x44)
    assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0, filename
    models = arm.call("HSD_ArchiveGetPublicAddress", archive, cstring(ROOT))
    assert models, filename
    model = u32(models)
    assert model, filename
    joint = u32(model)
    assert joint, filename
    root_jobj = arm.call("HSD_JObjLoadJoint", joint)
    assert root_jobj, filename

    # Runtime JObj tree also must not acquire a false particle flag.
    seen = set()
    stack = [root_jobj]
    runtime_count = 0
    while stack:
        jobj = stack.pop()
        if not jobj or jobj in seen:
            continue
        seen.add(jobj)
        runtime_count += 1
        flags = u32(jobj + 0x14)
        assert not (flags & JOBJ_PTCL), (filename, hex(jobj), hex(flags))
        stack.append(u32(jobj + 0x10))
        stack.append(u32(jobj + 0x08))
    assert runtime_count == len(expected_flags), (filename, runtime_count, len(expected_flags))
    arm.call("HSD_JObjRemoveAll", root_jobj)
    print(f"TRAINING_RAW_CASE {filename} joints={runtime_count} false_ptcl_fixed={false_ptcl}")

print(f"PASS GmTrain raw nativeization: cases={len(CASES)} joints=39 false_ptcl_fixed={total_false_ptcl}")
