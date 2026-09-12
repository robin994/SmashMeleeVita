#!/usr/bin/env python3
"""Linked-ARM regression for the stage-wide map_plit path used by lbshadow.

Fighter_Common does not use the callback-selected UnkStageDat_x8_t::x18
light set. ftCo_8009F4A4() calls Ground_801C49B4(), which gives a public
`map_plit` symbol precedence when present. This test nativeizes every retail
stage carrying that root, parses the archive, builds the real HSD_LObj list
through lb_80011AC4(), and applies lbshadow's exact 0x400/fallback selection.
"""

from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_CPSR,
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_R2,
)

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "orig/GALE01/files"
ELF = ROOT / "build/vita-full/melee_vita"


def parse_dat(raw: bytes):
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(
        ">4I", raw, 4
    )
    reloc_off = 32 + data_size
    public_off = reloc_off + reloc_count * 4
    strings_off = public_off + (public_count + extern_count) * 8
    reloc = {
        struct.unpack_from(">I", raw, reloc_off + i * 4)[0]
        for i in range(reloc_count)
    }
    public = {}
    for i in range(public_count):
        target, name_off = struct.unpack_from(">2I", raw, public_off + i * 8)
        start = strings_off + name_off
        end = raw.index(0, start)
        public[raw[start:end].decode("ascii", errors="replace")] = target
    return reloc, public


def source_map_plit(raw: bytes):
    reloc, public = parse_dat(raw)
    root = public.get("map_plit")
    if root is None:
        return None

    def be32(off):
        return struct.unpack_from(">I", raw, 32 + off)[0]

    def be16(off):
        return struct.unpack_from(">H", raw, 32 + off)[0]

    def ptr(field):
        return be32(field) if field in reloc else None

    out = []
    for list_index in range(64):
        light_list = ptr(root + list_index * 4)
        if light_list is None:
            return root, out
        desc = ptr(light_list)
        seen = set()
        while desc is not None:
            if desc in seen:
                raise AssertionError(("source LightDesc cycle", desc))
            seen.add(desc)
            out.append((be16(desc + 8), ptr(desc + 0x10) is not None))
            desc = ptr(desc + 4)
    raise AssertionError(("unterminated map_plit", root))


def select_shadow(lights):
    fallback = None
    selected = None
    for item in lights:
        if item[0] & 3:
            fallback = item
        if item[0] & 0x400:
            selected = item
            break
    return selected if selected is not None else fallback


def swapped_u16(value):
    return ((value & 0xFF) << 8) | (value >> 8)


def cstring(arm, text):
    raw = text.encode("ascii") + b"\0"
    ptr = arm.alloc(len(raw))
    arm.uc.mem_write(ptr, raw)
    return ptr


def u32(arm, addr):
    return struct.unpack("<I", arm.uc.mem_read(addr, 4))[0]


def u16(arm, addr):
    return struct.unpack("<H", arm.uc.mem_read(addr, 2))[0]


def call_long(arm, name, *args):
    try:
        return arm.call(name, *args)
    except AssertionError as exc:
        if str(exc) != f"{name}: did not return":
            raise
    for _ in range(12):
        pc = arm.uc.reg_read(UC_ARM_REG_PC)
        thumb = bool(arm.uc.reg_read(UC_ARM_REG_CPSR) & 32)
        arm.uc.emu_start(
            pc | thumb, arm.stop, timeout=30000000, count=100000000
        )
        if arm.uc.reg_read(UC_ARM_REG_PC) == arm.stop:
            return 0
    raise AssertionError(f"{name}: nativeizer timed out")


def make_arm():
    arm = ArmHarness(str(ELF))

    def silent(machine, _address, _size, _data):
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    def panic(machine, _address, _size, _data):
        msg = arm.string(machine.reg_read(UC_ARM_REG_R2)).decode(errors="replace")
        raise AssertionError("ARM HSD_Panic: " + msg)

    for name, hook in (("OSReport", silent), ("HSD_Panic", panic)):
        addr = arm.symbols[name] & ~1
        arm.uc.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)
    boot_component(arm)
    return arm


files = 0
source_lights = 0
runtime_lights = 0
pre_fix_bad = 0
grim_checked = False

for path in sorted(ASSETS.glob("Gr*.*")):
    raw = path.read_bytes()
    source = source_map_plit(raw)
    if source is None:
        continue
    root_offset, expected = source
    assert expected, (path.name, "empty map_plit")

    selected = select_shadow(expected)
    assert selected is not None and selected[1], (path.name, expected)
    wrong = select_shadow([(swapped_u16(flags), pos) for flags, pos in expected])
    if wrong is not None and not wrong[1]:
        pre_fix_bad += 1

    arm = make_arm()
    src = arm.alloc(len(raw))
    arm.uc.mem_write(src, raw)
    call_long(
        arm,
        "mv_stage_archive_prepare_raw",
        src,
        len(raw),
        cstring(arm, path.name),
    )

    archive = arm.alloc(0x44)
    assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0, path.name
    map_plit = arm.call(
        "HSD_ArchiveGetPublicAddress", archive, cstring(arm, "map_plit")
    )
    assert map_plit == src + 32 + root_offset, (
        path.name,
        hex(map_plit),
        hex(src + 32 + root_offset),
    )

    lobj = arm.call("lb_80011AC4", map_plit)
    assert lobj, (path.name, "lb_80011AC4 returned NULL")
    actual = []
    seen = set()
    cur = lobj
    while cur:
        assert cur not in seen, (path.name, "runtime LObj cycle")
        seen.add(cur)
        actual.append((u16(arm, cur + 8), bool(u32(arm, cur + 0x18)), cur))
        cur = u32(arm, cur + 0x0C)
        assert len(actual) <= 64, (path.name, "too many runtime lights")

    semantics = [(flags, has_pos) for flags, has_pos, _ in actual]
    assert semantics == expected, (path.name, expected, semantics)
    selected = select_shadow(actual)
    assert selected is not None and selected[1], (
        path.name,
        "lbshadow selected NULL-position map_plit light",
        semantics,
    )
    vec = arm.alloc(12)
    assert arm.call("HSD_LObjGetPosition", selected[2], vec) != 0, path.name

    if path.name == "GrIm.dat":
        assert expected == [(0x0004, False), (0x000D, True)], expected
        assert selected[0] == 0x000D and selected[1]
        grim_checked = True

    arm.call("HSD_LObjRemoveAll", lobj)
    files += 1
    source_lights += len(expected)
    runtime_lights += len(actual)

assert files == 72, files
assert source_lights == runtime_lights == 191, (source_lights, runtime_lights)
assert pre_fix_bad == files, (pre_fix_bad, files)
assert grim_checked
print(
    f"PASS stage map_plit/lbshadow: files={files} lights={runtime_lights}; "
    f"pre-fix endian path selects NULL-position light in {pre_fix_bad}/{files}; "
    "GrIm.dat selects 0x000d with position"
)