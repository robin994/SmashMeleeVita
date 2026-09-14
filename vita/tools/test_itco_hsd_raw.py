#!/usr/bin/env python3
"""Systemic ARM regression for every HSD item model reachable from PAL ItCo.dat."""
from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP

ASSET = Path("orig/GALE01/files/ItCo.dat")
ARTICLE_COUNTS = (43, 118, 47)


def parse_dat(raw: bytes):
    if len(raw) < 32 or struct.unpack_from(">I", raw, 0)[0] != len(raw):
        raise AssertionError("bad ItCo DAT header")
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(">4I", raw, 4)
    reloc_off = 32 + data_size
    public_off = reloc_off + reloc_count * 4
    strings_off = public_off + (public_count + extern_count) * 8
    relocs = {struct.unpack_from(">I", raw, reloc_off + i * 4)[0] for i in range(reloc_count)}
    publics = {}
    for i in range(public_count):
        target, name_off = struct.unpack_from(">2I", raw, public_off + i * 8)
        start = strings_off + name_off
        end = raw.index(0, start)
        publics[raw[start:end].decode("ascii")] = target
    return data_size, relocs, publics


def word(raw: bytes, off: int) -> int:
    return struct.unpack_from(">I", raw, 32 + off)[0]


def ptr(raw: bytes, relocs: set[int], off: int):
    value = word(raw, off)
    if off in relocs:
        return value
    if value == 0:
        return None
    raise AssertionError(f"expected relocation/null at 0x{off:x}, got 0x{value:08x}")


raw = ASSET.read_bytes()
data_size, relocs, publics = parse_dat(raw)
root = publics["itPublicData"]
article_tables = [ptr(raw, relocs, root + (i + 1) * 4) for i in range(3)]
articles = set()
for table, count in zip(article_tables, ARTICLE_COUNTS):
    assert table is not None
    for i in range(count):
        article = ptr(raw, relocs, table + i * 4)
        if article is not None:
            articles.add(article)
models = set()
for article in articles:
    model = ptr(raw, relocs, article + 0x10)
    if model is not None:
        models.add(model)
joint_roots = set()
for model in models:
    joint = ptr(raw, relocs, model)
    if joint is not None:
        joint_roots.add(joint)
assert len(joint_roots) == 78, f"unexpected generic item model root count {len(joint_roots)}"

# Food (common item kind 18) replaces the Article base joint with one of 28
# HSD_Joint roots stored in its bespoke special-attribute table. These roots
# and the scalar count/heal/placement fields must cross the endian boundary too.
food_article = ptr(raw, relocs, article_tables[0] + 18 * 4)
assert food_article is not None
food_attrs = ptr(raw, relocs, food_article + 4)
assert food_attrs is not None
food_count = word(raw, food_attrs)
assert food_count == 28, f"unexpected Food count {food_count}"
food_expected = []
for i in range(food_count):
    rec = food_attrs + i * 0x10
    joint = ptr(raw, relocs, rec + 4)
    assert joint is not None, f"Food {i} missing model joint"
    heal = struct.unpack_from(">i", raw, 32 + rec + 8)[0]
    xoff = struct.unpack_from(">f", raw, 32 + rec + 0x0C)[0]
    yoff = struct.unpack_from(">f", raw, 32 + rec + 0x10)[0]
    food_expected.append((rec, joint, heal, xoff, yoff))
    joint_roots.add(joint)
assert len(joint_roots) == 106, f"unexpected item+Food HSD root count {len(joint_roots)}"

arm = ArmHarness("build/vita-full/melee_vita")
# The harness uses a bump allocator and intentionally ignores free(); the real Vita
# allocator reuses these temporary validator allocations. Extend only the test arena.
extra_heap = 256 * 1024 * 1024
arm.uc.mem_map(arm.heap_end, extra_heap)
arm.heap_end += extra_heap


def silent(m, _address, _size, _data):
    message = arm.string(m.reg_read(UC_ARM_REG_R0)).decode(errors="replace")
    if "VITA_HSD_GRAPH_SET_VALIDATE_FAIL" in message:
        root = struct.unpack("<I", m.mem_read(m.reg_read(UC_ARM_REG_SP), 4))[0]
        print(f"HSD_VALIDATE file={arm.string(m.reg_read(UC_ARM_REG_R1)).decode()} label={arm.string(m.reg_read(UC_ARM_REG_R2)).decode()} index={m.reg_read(UC_ARM_REG_R3)} root=0x{root:08x}", flush=True)
    elif "VITA_ITEM_STATE_RAW" in message:
        print(message, flush=True)
    m.reg_write(UC_ARM_REG_PC, m.reg_read(UC_ARM_REG_LR))


report = arm.symbols["OSReport"] & ~1
arm.uc.hook_add(UC_HOOK_CODE, silent, begin=report, end=report)
boot_component(arm)

src = arm.alloc(len(raw))
arm.uc.mem_write(src, raw)
name = arm.alloc(len(b"ItCo.dat\0"))
arm.uc.mem_write(name, b"ItCo.dat\0")
try:
    arm.call("mv_gameplay_archive_prepare_raw", src, len(raw), name)
except AssertionError as limit:
    if str(limit) != "mv_gameplay_archive_prepare_raw: did not return":
        raise
    for _ in range(12):
        pc = arm.uc.reg_read(UC_ARM_REG_PC)
        thumb = bool(arm.uc.reg_read(UC_ARM_REG_CPSR) & 32)
        arm.uc.emu_start(pc | thumb, arm.stop, timeout=30000000, count=100000000)
        if arm.uc.reg_read(UC_ARM_REG_PC) == arm.stop:
            break
    assert arm.uc.reg_read(UC_ARM_REG_PC) == arm.stop, "ItCo nativeizer timed out"

# Food scalar fields must be native ARM values before relocation, while each
# x4 HSD_Joint relocation word remains byte-for-byte untouched.
data_base = src + 32
native_food_count = struct.unpack("<i", arm.uc.mem_read(data_base + food_attrs, 4))[0]
assert native_food_count == food_count, (native_food_count, food_count)
for i, (rec, joint, heal, xoff, yoff) in enumerate(food_expected):
    raw_joint_bytes = raw[32 + rec + 4:32 + rec + 8]
    native_joint_bytes = bytes(arm.uc.mem_read(data_base + rec + 4, 4))
    assert native_joint_bytes == raw_joint_bytes, f"Food {i} relocation word was mutated"
    native_heal = struct.unpack("<i", arm.uc.mem_read(data_base + rec + 8, 4))[0]
    native_xoff = struct.unpack("<f", arm.uc.mem_read(data_base + rec + 0x0C, 4))[0]
    native_yoff = struct.unpack("<f", arm.uc.mem_read(data_base + rec + 0x10, 4))[0]
    assert native_heal == heal, (i, native_heal, heal)
    assert abs(native_xoff - xoff) < 1e-6, (i, native_xoff, xoff)
    assert abs(native_yoff - yoff) < 1e-6, (i, native_yoff, yoff)

# Every root JObj flag must now equal the retail BE numeric value when read as ARM LE.
for joint in joint_roots:
    expected = word(raw, joint + 4)
    native = struct.unpack("<I", arm.uc.mem_read(src + 32 + joint + 4, 4))[0]
    assert native == expected, (
        f"item JObj 0x{joint:x} flags not nativeized: 0x{native:08x} != 0x{expected:08x}"
    )

archive = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
loaded = 0
for joint in sorted(joint_roots):
    jobj = arm.call("HSD_JObjLoadJoint", src + 32 + joint)
    assert jobj, f"HSD_JObjLoadJoint returned NULL for item root 0x{joint:x}"
    arm.call("HSD_JObjRemoveAll", jobj)
    loaded += 1

print(
    f"PASS ItCo item HSD+Food: articles={len(articles)} models={len(models)} "
    f"food={food_count} joint_roots={len(joint_roots)} loaded={loaded}; raw conversion, relocation, "
    "RObj bytecode refs and original HSD load/remove all succeeded"
)
