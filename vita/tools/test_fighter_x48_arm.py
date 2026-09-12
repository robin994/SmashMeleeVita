#!/usr/bin/env python3
"""ARM regression for fighter-owned ftData.x48 item/accessory HSD data."""
from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC

ASSETS = Path("orig/GALE01/files")
ARTICLE_MASKS = {
    "ftDataCrazyhand": 0x007, "ftDataClink": 0x03F,
    "ftDataDrmario": 0x00A, "ftDataFalco": 0x00B,
    "ftDataFox": 0x007, "ftDataGamewatch": 0x3FF,
    "ftDataGkoopa": 0x001, "ftDataKirby": 0x00F,
    "ftDataKoopa": 0x001, "ftDataLink": 0x01F,
    "ftDataLuigi": 0x001, "ftDataMario": 0x005,
    "ftDataMasterhand": 0x003, "ftDataMewtwo": 0x003,
    "ftDataNess": 0x7FF, "ftDataPeach": 0x01F,
    "ftDataPichu": 0x007, "ftDataPikachu": 0x007,
    "ftDataPopo": 0x007, "ftDataSamus": 0x00F,
    "ftDataSeak": 0x00F, "ftDataYoshi": 0x007,
    "ftDataZelda": 0x003,
}
DIRECT_JOINT_INDICES = {
    "ftDataClink": (6,), "ftDataLink": (6,), "ftDataKirby": (4,),
    "ftDataSeak": (4, 5), "ftDataYoshi": (3,),
}
# Samus x48[4] is an accessory bundle whose first field is a JObj graph.  Its
# retail tree contains genuine JOBJ_INSTANCE nodes, so loading this root is the
# regression that proves Vita's ID-table instance resolution works rather than
# merely masking the old false-endian INSTANCE/PTCL flags.
INSTANCE_BUNDLE_INDICES = {"ftDataSamus": (4,)}

# Exercise the exact v3.53 crash family plus the entire source-typed x48
# Article/direct-JObj roster and the genuine-instance Samus accessory graph.
CASES = tuple(sorted(
    p.name for p in ASSETS.glob("Pl??.dat") if p.name != "PlCo.dat"
))


def parse_dat(raw: bytes):
    assert len(raw) >= 32 and struct.unpack_from(">I", raw, 0)[0] == len(raw)
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


def ptr(raw: bytes, relocs: set[int], off: int, *, allow_minus_one=False):
    value = word(raw, off)
    if off in relocs:
        return value
    if value == 0 or (allow_minus_one and value == 0xFFFFFFFF):
        return None
    raise AssertionError(f"expected relocation/null at 0x{off:x}, got 0x{value:08x}")


def collect(raw: bytes):
    _, relocs, publics = parse_dat(raw)
    names = [name for name in publics if name.startswith("ftData")]
    assert len(names) == 1
    root_name = names[0]
    root = publics[root_name]
    table = ptr(raw, relocs, root + 0x48)
    mask = ARTICLE_MASKS.get(root_name, 0)
    roots = set()
    articles = 0
    if mask:
        assert table is not None
        for i in range(32):
            if not (mask & (1 << i)):
                continue
            article = ptr(raw, relocs, table + i * 4)
            assert article is not None
            articles += 1
            for field in range(6):
                ptr(raw, relocs, article + field * 4)
            model = ptr(raw, relocs, article + 0x10)
            if model is not None:
                joint = ptr(raw, relocs, model, allow_minus_one=True)
                if joint is not None:
                    roots.add(joint)
    for i in DIRECT_JOINT_INDICES.get(root_name, ()):
        joint = ptr(raw, relocs, table + i * 4)
        assert joint is not None
        roots.add(joint)
    instance_roots = set()
    for i in INSTANCE_BUNDLE_INDICES.get(root_name, ()):
        bundle = ptr(raw, relocs, table + i * 4)
        assert bundle is not None
        joint = ptr(raw, relocs, bundle)
        assert joint is not None
        roots.add(joint)
        instance_roots.add(joint)
    return root_name, relocs, roots, instance_roots, articles


arm = ArmHarness("build/vita-full/melee_vita")
extra_heap = 320 * 1024 * 1024
arm.uc.mem_map(arm.heap_end, extra_heap)
arm.heap_end += extra_heap


def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


report = arm.symbols["OSReport"] & ~1
arm.uc.hook_add(UC_HOOK_CODE, silent, begin=report, end=report)
boot_component(arm)

loaded = 0
checked_articles = 0
checked_roots = 0
checked_cases = 0
checked_instance_roots = 0
for filename in CASES:
    raw = (ASSETS / filename).read_bytes()
    root_name, relocs, roots, instance_roots, articles = collect(raw)
    if (root_name not in ARTICLE_MASKS and
        root_name not in DIRECT_JOINT_INDICES and
        root_name not in INSTANCE_BUNDLE_INDICES):
        continue
    checked_cases += 1
    checked_articles += articles
    checked_roots += len(roots)
    checked_instance_roots += len(instance_roots)
    src = arm.alloc(len(raw))
    arm.uc.mem_write(src, raw)
    name_bytes = filename.encode("ascii") + b"\0"
    name = arm.alloc(len(name_bytes))
    arm.uc.mem_write(name, name_bytes)
    arm.call("mv_gameplay_archive_prepare_raw", src, len(raw), name)

    for joint in roots:
        expected = word(raw, joint + 4)
        native = struct.unpack("<I", arm.uc.mem_read(src + 32 + joint + 4, 4))[0]
        assert native == expected, (
            f"{root_name} x48 JObj 0x{joint:x} flags 0x{native:08x} != 0x{expected:08x}"
        )
        assert not (native & 0x20), (
            f"{root_name} has unsupported PTCL x48 root flags 0x{native:08x}"
        )

    archive = arm.alloc(0x44)
    assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
    for joint in sorted(roots):
        jobj = arm.call("HSD_JObjLoadJoint", src + 32 + joint)
        assert jobj, f"{root_name}: HSD_JObjLoadJoint NULL at 0x{joint:x}"
        arm.call("HSD_JObjRemoveAll", jobj)
        loaded += 1

assert checked_cases == 23, f"x48 fighter census drift: {checked_cases}"
assert checked_articles == 83, f"x48 Article census drift: {checked_articles}"
assert checked_roots == 75, f"x48 supported joint-root census drift: {checked_roots}"
assert checked_instance_roots == 1, (
    f"x48 INSTANCE accessory-root census drift: {checked_instance_roots}"
)
assert loaded == checked_roots

print(
    f"PASS fighter x48 HSD: cases={checked_cases} articles={checked_articles} "
    f"joint_roots={checked_roots} instance_roots={checked_instance_roots} loaded={loaded}; "
    "Fox Article, the full supported x48 roster and Samus' genuine INSTANCE accessory "
    "nativeize/load with PTCL still fail-closed"
)
