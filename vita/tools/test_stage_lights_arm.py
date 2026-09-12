#!/usr/bin/env python3
"""Linked-ARM regression for the stage map light path consumed by lbshadow.

This exercises the real serialized chain:
  map_head -> UnkStageDat_x8_t.x18 -> LightList** -> HSD_LightDesc
  -> lb_80011AC4 -> HSD_LObj -> lbshadow selection semantics.

Pre-fix stage nativeizers converted map JObjs but not the full light graph, so
retail light sets could read BE u16 flags backwards on ARM. In particular 0x0004
(ambient) became 0x0400 (the shadow-light marker) while retaining no position.
"""

from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR, UC_ARM_REG_R2

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "orig/GALE01/files"
ELF = ROOT / "build/vita-full/melee_vita"


def parse_dat(raw: bytes):
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(">4I", raw, 4)
    data_off = 32
    reloc_off = data_off + data_size
    public_off = reloc_off + reloc_count * 4
    strings_off = public_off + (public_count + extern_count) * 8
    reloc = {struct.unpack_from(">I", raw, reloc_off + i * 4)[0]
             for i in range(reloc_count)}
    public = {}
    for i in range(public_count):
        target, name_off = struct.unpack_from(">2I", raw, public_off + i * 8)
        start = strings_off + name_off
        end = raw.index(0, start)
        public[raw[start:end].decode("ascii", errors="replace")] = target
    return data_off, reloc, public


def source_stage_lights(raw: bytes):
    data_off, reloc, public = parse_dat(raw)
    if "map_head" not in public:
        return []

    def be32(off):
        return struct.unpack_from(">I", raw, data_off + off)[0]

    def be16(off):
        return struct.unpack_from(">H", raw, data_off + off)[0]

    def ptr(field):
        return be32(field) if field in reloc else None

    map_head = public["map_head"]
    maps = ptr(map_head + 8)
    count = be32(map_head + 0x0C)
    if maps is None or count > 0x400:
        return []

    out = []
    for map_index in range(count):
        entry = maps + map_index * 0x34
        light_table = ptr(entry + 0x18)
        if light_table is None:
            continue
        lights = []
        for list_index in range(64):
            list_desc = ptr(light_table + list_index * 4)
            if list_desc is None:
                break
            desc = ptr(list_desc)
            seen = set()
            while desc is not None:
                if desc in seen:
                    raise AssertionError(("light desc cycle", desc))
                seen.add(desc)
                flags = be16(desc + 8)
                position = ptr(desc + 0x10)
                lights.append((flags, position is not None, desc))
                desc = ptr(desc + 4)
        else:
            raise AssertionError(("unterminated light table", map_index, light_table))
        out.append((map_index, maps, light_table, lights))
    return out


def select_shadow(lights):
    fallback = None
    selected = None
    for item in lights:
        flags = item[0]
        if flags & 3:
            fallback = item
        if flags & 0x400:
            selected = item
            break
    return selected if selected is not None else fallback


def swapped_u16(value):
    return ((value & 0xFF) << 8) | (value >> 8)


def cstring(arm, text: str):
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
        arm.uc.emu_start(pc | thumb, arm.stop, timeout=30000000, count=100000000)
        if arm.uc.reg_read(UC_ARM_REG_PC) == arm.stop:
            return 0  # return value is irrelevant here
    raise AssertionError(f"{name}: nativeizer timed out")


def make_arm():
    arm = ArmHarness(str(ELF))

    def silent(machine, _address, _size, _data):
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    def panic(machine, _address, _size, _data):
        msg = arm.string(machine.reg_read(UC_ARM_REG_R2)).decode(errors="replace")
        raise AssertionError("ARM HSD_Panic: " + msg)

    for name, hook in (("OSReport", silent), ("HSD_Panic", panic)):
        if name in arm.symbols:
            addr = arm.symbols[name] & ~1
            arm.uc.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)
    boot_component(arm)
    return arm


source_sets = 0
source_lights = 0
pre_fix_bad_sets = 0
runtime_sets = 0
runtime_lights = 0
grz6_checked = False

for path in sorted(ASSETS.glob("Gr*.*")):
    raw = path.read_bytes()
    sets = source_stage_lights(raw)
    if not sets:
        continue

    # Prove the failure mode from the retail bytes before running the fixed
    # binary. Every legal source light-set must select a positioned light; the
    # v3.56 little-endian interpretation must demonstrate the old bad path.
    for map_index, _maps, _table, lights in sets:
        selected = select_shadow(lights)
        assert selected is not None and selected[1], (path.name, map_index, lights)
        wrong = select_shadow([(swapped_u16(f), pos, off) for f, pos, off in lights])
        if wrong is not None and not wrong[1]:
            pre_fix_bad_sets += 1
        source_sets += 1
        source_lights += len(lights)

    arm = make_arm()
    src = arm.alloc(len(raw))
    arm.uc.mem_write(src, raw)
    call_long(arm, "mv_stage_archive_prepare_raw", src, len(raw), cstring(arm, path.name))
    archive = arm.alloc(0x44)
    assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0, path.name

    data_off, _reloc, public = parse_dat(raw)
    map_head = src + data_off + public["map_head"]
    maps = u32(arm, map_head + 8)

    for map_index, _source_maps, _source_table, expected in sets:
        entry = maps + map_index * 0x34
        light_table = u32(arm, entry + 0x18)
        assert light_table, (path.name, map_index, "relocated x18 missing")
        lobj = arm.call("lb_80011AC4", light_table)
        assert lobj, (path.name, map_index, "lb_80011AC4 returned NULL")

        actual = []
        cur = lobj
        seen = set()
        while cur:
            assert cur not in seen, (path.name, map_index, "runtime LObj cycle")
            seen.add(cur)
            actual.append((u16(arm, cur + 8), bool(u32(arm, cur + 0x18)), cur))
            cur = u32(arm, cur + 0x0C)
            assert len(actual) <= 64, (path.name, map_index, "too many runtime lights")

        expected_semantics = [(flags, has_pos) for flags, has_pos, _ in expected]
        actual_semantics = [(flags, has_pos) for flags, has_pos, _ in actual]
        assert actual_semantics == expected_semantics, (
            path.name, map_index, "stage light semantic mismatch",
            expected_semantics, actual_semantics,
        )

        selected = select_shadow(actual)
        assert selected is not None, (path.name, map_index, "no lbshadow light")
        assert selected[1], (path.name, map_index, "lbshadow selected NULL-position light", actual_semantics)

        # Exact hardware crash regression. GrZe map 6 is the map selected in
        # runtime.log from v3.56; source is ambient 0x0004 + point 0x000e.
        if path.name == "GrZe.dat" and map_index == 6:
            assert expected_semantics == [(0x0004, False), (0x000E, True)], expected_semantics
            assert actual_semantics == expected_semantics, actual_semantics
            assert selected[0] == 0x000E and selected[1]
            grz6_checked = True

        arm.call("HSD_LObjRemoveAll", lobj)
        runtime_sets += 1
        runtime_lights += len(actual)

assert source_sets == runtime_sets
assert source_lights == runtime_lights
assert pre_fix_bad_sets == source_sets, (pre_fix_bad_sets, source_sets)
assert grz6_checked
print(
    f"PASS stage light/lbshadow: sets={runtime_sets} lights={runtime_lights}; "
    f"v3.56 endian path would select NULL-position light in {pre_fix_bad_sets}/{source_sets} sets; "
    "GrZe.dat map6 runtime flags=0004,000e and shadow selection=000e with position"
)
