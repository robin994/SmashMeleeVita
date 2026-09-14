#!/usr/bin/env python3
"""Linked-ARM regression for every retail fighter costume HSD archive."""

from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC

ASSETS = Path("orig/GALE01/files")
JOBJ_PTCL = 0x20
JOBJ_INSTANCE = 0x1000


def parse_dat(raw: bytes):
    assert len(raw) >= 32 and struct.unpack_from(">I", raw, 0)[0] == len(raw)
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(">4I", raw, 4)
    reloc_base = 32 + data_size
    public_base = reloc_base + reloc_count * 4
    strings_base = public_base + (public_count + extern_count) * 8
    relocs = {
        struct.unpack_from(">I", raw, reloc_base + i * 4)[0]
        for i in range(reloc_count)
    }
    publics = {}
    for i in range(public_count):
        target, name_off = struct.unpack_from(">2I", raw, public_base + i * 8)
        start = strings_base + name_off
        end = raw.index(0, start)
        publics[raw[start:end].decode("ascii")] = target
    return relocs, publics


def be32(raw: bytes, off: int) -> int:
    return struct.unpack_from(">I", raw, 32 + off)[0]


def bswap32(value: int) -> int:
    return struct.unpack("<I", struct.pack(">I", value))[0]


def costume_root(publics):
    roots = [
        (name, target)
        for name, target in publics.items()
        if name.startswith("Ply")
        and "5K" in name
        and name.endswith("_Share_joint")
    ]
    if not roots:
        return None
    assert len(roots) == 1, roots
    return roots[0]


def collect_joint_flags(raw: bytes, relocs: set[int], root: int):
    seen = set()
    flags = {}

    def ptr(off: int):
        return be32(raw, off) if off in relocs else 0

    def walk(joint: int):
        if not joint or joint in seen:
            return
        seen.add(joint)
        value = be32(raw, joint + 4)
        flags[joint] = value
        if not (value & JOBJ_INSTANCE):
            walk(ptr(joint + 8))
        walk(ptr(joint + 12))

    walk(root)
    return flags


cases = []
for path in sorted(ASSETS.glob("Pl*.dat")):
    raw = path.read_bytes()
    # Fighter animation banks (Pl??AJ.dat) are Figatree containers, not HSD DATs.
    if len(raw) < 32 or struct.unpack_from(">I", raw, 0)[0] != len(raw):
        continue
    relocs, publics = parse_dat(raw)
    root = costume_root(publics)
    if root is not None:
        cases.append((path, raw, relocs, root))

assert len(cases) == 125, f"fighter costume archive census drift: {len(cases)}"
default_cases = sum(name.endswith("5K_Share_joint") for _, _, _, (name, _) in cases)
variant_cases = len(cases) - default_cases
assert default_cases == 28, f"default costume census drift: {default_cases}"
assert variant_cases == 97, f"alternate costume census drift: {variant_cases}"

arm = ArmHarness("build/vita-full/melee_vita")
extra_heap = 320 * 1024 * 1024
arm.uc.mem_map(arm.heap_end, extra_heap)
arm.heap_end += extra_heap


def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


report = arm.symbols["OSReport"] & ~1
arm.uc.hook_add(UC_HOOK_CODE, silent, begin=report, end=report)
boot_component(arm)


def cstring(text: str):
    raw = text.encode("ascii") + b"\0"
    ptr = arm.alloc(len(raw))
    arm.uc.mem_write(ptr, raw)
    return ptr


def u32(addr: int) -> int:
    return struct.unpack("<I", arm.uc.mem_read(addr, 4))[0]


loaded = 0
joints_checked = 0
false_ptcl_fixed = 0
for path, raw, relocs, (root_name, root_offset) in cases:
    expected_flags = collect_joint_flags(raw, relocs, root_offset)
    assert expected_flags, (path.name, root_name)
    assert all(not (value & JOBJ_PTCL) for value in expected_flags.values()), (
        path.name, "genuine JOBJ_PTCL unexpectedly present"
    )
    false_ptcl_fixed += sum(
        1
        for value in expected_flags.values()
        if bswap32(value) & JOBJ_PTCL
    )

    src = arm.alloc(len(raw))
    arm.uc.mem_write(src, raw)
    arm.call("mv_boot_archive_prepare_raw", src, len(raw), cstring(path.name))

    # Every reachable serialized JObj flag must now be native ARM endian.
    for offset, expected in expected_flags.items():
        actual = u32(src + 32 + offset + 4)
        assert actual == expected, (
            path.name,
            root_name,
            hex(offset),
            hex(actual),
            hex(expected),
        )
        assert not (actual & JOBJ_PTCL), (path.name, root_name, hex(offset), hex(actual))
    joints_checked += len(expected_flags)

    archive = arm.alloc(0x44)
    assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0, path.name
    joint = arm.call("HSD_ArchiveGetPublicAddress", archive, cstring(root_name))
    assert joint, (path.name, root_name)
    root_jobj = arm.call("HSD_JObjLoadJoint", joint)
    assert root_jobj, (path.name, root_name)
    arm.call("HSD_JObjRemoveAll", root_jobj)
    loaded += 1

assert loaded == len(cases)
assert false_ptcl_fixed > 0
print(
    "PASS fighter costume HSD: "
    f"archives={len(cases)} default={default_cases} alternates={variant_cases} "
    f"joints={joints_checked} false_ptcl_fixed={false_ptcl_fixed} loaded={loaded}; "
    "all retail costume JObj trees nativeize/relocate/load/remove with genuine PTCL still fail-closed"
)
