#!/usr/bin/env python3
"""Systemic ARM regression for every HSD item model reachable from PAL ItCo.dat."""
from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC

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
assert len(joint_roots) == 78, f"unexpected unique item model root count {len(joint_roots)}"

arm = ArmHarness("build/vita-full/melee_vita")
# The harness uses a bump allocator and intentionally ignores free(); the real Vita
# allocator reuses these temporary validator allocations. Extend only the test arena.
extra_heap = 256 * 1024 * 1024
arm.uc.mem_map(arm.heap_end, extra_heap)
arm.heap_end += extra_heap


def silent(m, _address, _size, _data):
    m.reg_write(UC_ARM_REG_PC, m.reg_read(UC_ARM_REG_LR))


report = arm.symbols["OSReport"] & ~1
arm.uc.hook_add(UC_HOOK_CODE, silent, begin=report, end=report)
boot_component(arm)

src = arm.alloc(len(raw))
arm.uc.mem_write(src, raw)
name = arm.alloc(len(b"ItCo.dat\0"))
arm.uc.mem_write(name, b"ItCo.dat\0")
arm.call("mv_gameplay_archive_prepare_raw", src, len(raw), name)

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
    f"PASS ItCo item HSD: articles={len(articles)} models={len(models)} "
    f"joint_roots={len(joint_roots)} loaded={loaded}; raw conversion, relocation, "
    "RObj bytecode refs and original HSD load/remove all succeeded"
)
