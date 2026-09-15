#!/usr/bin/env python3
"""ARM regression for Kirby copy-ability DAT roots (PlKbCp*.dat)."""

from pathlib import Path
import argparse
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3


KIRBY_COPY_HAT = 0
KIRBY_COPY_COSTUME = 1

# (layout, source-derived Article bitmask).  The bitmask mirrors
# ftKb_SpecialN_800F16D0 and deliberately leaves unknown hat_dynamics slots
# untouched.
SCHEMA = {
    "ftDataKirbyCopyMario": (KIRBY_COPY_HAT, 0x01),
    "ftDataKirbyCopyFox": (KIRBY_COPY_HAT, 0x03),
    "ftDataKirbyCopyCaptain": (KIRBY_COPY_HAT, 0x00),
    "ftDataKirbyCopyDonkey": (KIRBY_COPY_COSTUME, 0x00),
    "ftDataKirbyCopyKoopa": (KIRBY_COPY_HAT, 0x01),
    "ftDataKirbyCopyLink": (KIRBY_COPY_HAT, 0x03),
    "ftDataKirbyCopySeak": (KIRBY_COPY_HAT, 0x03),
    "ftDataKirbyCopyNess": (KIRBY_COPY_HAT, 0x03),
    "ftDataKirbyCopyPeach": (KIRBY_COPY_HAT, 0x03),
    "ftDataKirbyCopyPopo": (KIRBY_COPY_HAT, 0x01),
    "ftDataKirbyCopyPikachu": (KIRBY_COPY_HAT, 0x03),
    "ftDataKirbyCopySamus": (KIRBY_COPY_HAT, 0x01),
    "ftDataKirbyCopyYoshi": (KIRBY_COPY_HAT, 0x20),
    "ftDataKirbyCopyPurin": (KIRBY_COPY_COSTUME, 0x00),
    "ftDataKirbyCopyMewtwo": (KIRBY_COPY_COSTUME, 0x08),
    "ftDataKirbyCopyLuigi": (KIRBY_COPY_HAT, 0x01),
    "ftDataKirbyCopyMars": (KIRBY_COPY_HAT, 0x00),
    "ftDataKirbyCopyZelda": (KIRBY_COPY_HAT, 0x00),
    "ftDataKirbyCopyClink": (KIRBY_COPY_HAT, 0x03),
    "ftDataKirbyCopyDrmario": (KIRBY_COPY_HAT, 0x01),
    "ftDataKirbyCopyFalco": (KIRBY_COPY_COSTUME, 0x18),
    "ftDataKirbyCopyPichu": (KIRBY_COPY_HAT, 0x03),
    "ftDataKirbyCopyGamewatch": (KIRBY_COPY_COSTUME, 0x60),
    "ftDataKirbyCopyGanon": (KIRBY_COPY_HAT, 0x00),
    "ftDataKirbyCopyEmblem": (KIRBY_COPY_HAT, 0x00),
}


def parse_dat(raw: bytes):
    assert len(raw) >= 32 and struct.unpack_from(">I", raw, 0)[0] == len(raw)
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(">4I", raw, 4)
    reloc_off = 32 + data_size
    public_off = reloc_off + reloc_count * 4
    strings_off = public_off + (public_count + extern_count) * 8
    relocs = {
        struct.unpack_from(">I", raw, reloc_off + i * 4)[0]
        for i in range(reloc_count)
    }
    publics = {}
    for i in range(public_count):
        target, name_off = struct.unpack_from(">2I", raw, public_off + i * 8)
        start = strings_off + name_off
        end = raw.index(0, start)
        publics[raw[start:end].decode("ascii")] = target
    return relocs, publics


def word(raw: bytes, off: int) -> int:
    return struct.unpack_from(">I", raw, 32 + off)[0]


def ptr(raw: bytes, relocs: set[int], off: int, *, allow_minus_one=False):
    value = word(raw, off)
    if off in relocs:
        return value
    if value == 0 or (allow_minus_one and value == 0xFFFFFFFF):
        return None
    raise AssertionError(
        f"expected relocation/null at 0x{off:x}, got 0x{value:08x}"
    )


def collect_case(raw: bytes):
    relocs, publics = parse_dat(raw)
    names = [name for name in publics if name.startswith("ftDataKirbyCopy")]
    assert len(names) == 1, names
    root_name = names[0]
    assert root_name in SCHEMA, root_name
    layout, article_mask = SCHEMA[root_name]
    root = publics[root_name]
    hsd_roots = set()

    if layout == KIRBY_COPY_HAT:
        joint = ptr(raw, relocs, root)
        assert joint is not None
        hsd_roots.add(joint)
        assert 0 < word(raw, root + 4) <= 11
        assert ptr(raw, relocs, root + 8) is not None
    else:
        assert 0 < word(raw, root) <= 11
        assert ptr(raw, relocs, root + 4) is not None
        assert word(raw, root + 8) <= 5
        assert ptr(raw, relocs, root + 0x0C) is not None
        extra_joint = ptr(raw, relocs, root + 0x14)
        if extra_joint is not None:
            hsd_roots.add(extra_joint)

    article_count = 0
    for index in range(32):
        if not (article_mask & (1 << index)):
            continue
        article = ptr(raw, relocs, root + 0x0C + index * 4)
        assert article is not None
        article_count += 1
        # Article +0x10 is ItemModelDesc*.  Collect its JObj root so the test
        # validates the same HSD graphs the nativeizer follows.
        model = ptr(raw, relocs, article + 0x10)
        if model is not None:
            joint = ptr(raw, relocs, model, allow_minus_one=True)
            if joint is not None:
                hsd_roots.add(joint)

    return root_name, root, layout, article_count, hsd_roots


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", default="build/vita-full/melee_vita")
    parser.add_argument("--assets", default="orig/GALE01/files")
    args = parser.parse_args()

    assets = Path(args.assets)
    cases = sorted(assets.glob("PlKbCp*.dat"))
    assert len(cases) == 25, f"Kirby copy DAT census drift: {len(cases)}"

    arm = ArmHarness(args.elf)
    extra_heap = 384 * 1024 * 1024
    arm.uc.mem_map(arm.heap_end, extra_heap)
    arm.heap_end += extra_heap
    boot_component(arm)

    # Keep conversion failures actionable: OSReport normally disappears into
    # the harness vfprintf stub, so decode the schema-failure arguments here.
    def report_hook(machine, _address, _size, _data):
        fmt = arm.string(machine.reg_read(UC_ARM_REG_R0)).decode(errors="replace")
        if fmt.startswith("VITA_GAMEPLAY_DAT_INVALID"):
            kind = arm.string(machine.reg_read(UC_ARM_REG_R1)).decode(errors="replace")
            detail = arm.string(machine.reg_read(UC_ARM_REG_R2)).decode(errors="replace")
            off = machine.reg_read(UC_ARM_REG_R3)
            print(f"SCHEMA_FAIL kind={kind} detail={detail} off=0x{off:08x}", flush=True)

    if "OSReport" in arm.symbols:
        report = arm.symbols["OSReport"] & ~1
        arm.uc.hook_add(UC_HOOK_CODE, report_hook, begin=report, end=report)

    seen_names = set()
    converted_articles = 0
    loaded_roots = 0
    for path in cases:
        raw = path.read_bytes()
        root_name, root, layout, articles, hsd_roots = collect_case(raw)
        seen_names.add(root_name)
        converted_articles += articles

        src = arm.alloc(len(raw))
        arm.uc.mem_write(src, raw)
        filename_raw = path.name.encode("ascii") + b"\0"
        filename = arm.alloc(len(filename_raw))
        arm.uc.mem_write(filename, filename_raw)
        print(f"CHECK {path.name} {root_name}", flush=True)
        arm.call("mv_gameplay_archive_prepare_raw", src, len(raw), filename)

        if layout == KIRBY_COPY_HAT:
            native_model_num = struct.unpack(
                "<I", arm.uc.mem_read(src + 32 + root + 4, 4)
            )[0]
            assert native_model_num == word(raw, root + 4), root_name
        else:
            for off in (0, 8, 0x10):
                native_value = struct.unpack(
                    "<I", arm.uc.mem_read(src + 32 + root + off, 4)
                )[0]
                assert native_value == word(raw, root + off), (
                    root_name, off, native_value
                )

        archive = arm.alloc(0x44)
        assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
        for joint in sorted(hsd_roots):
            jobj = arm.call("HSD_JObjLoadJoint", src + 32 + joint)
            assert jobj, f"{root_name}: HSD_JObjLoadJoint NULL at 0x{joint:x}"
            arm.call("HSD_JObjRemoveAll", jobj)
            loaded_roots += 1

    assert seen_names == set(SCHEMA), (
        f"Kirby copy schema census mismatch missing={set(SCHEMA)-seen_names} "
        f"extra={seen_names-set(SCHEMA)}"
    )
    print(
        f"PASS Kirby copy raw: files={len(cases)} articles={converted_articles} "
        f"hsd_roots_loaded={loaded_roots}; compact copy roots no longer enter "
        "the FighterData converter"
    )


if __name__ == "__main__":
    main()
