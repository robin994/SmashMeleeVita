#!/usr/bin/env python3
"""Verify Vita HSD release callbacks reclaim retail JObj graphs without heap drift."""

from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC


ASSET = Path("orig/GALE01/files/GmPause.dat")
PUBLIC = "ScGamPause_scene_data"
ITERATIONS = 24


def parse_public(raw: bytes, wanted: str) -> int:
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


raw = ASSET.read_bytes()
scene_off = parse_public(raw, PUBLIC)
arm = ArmHarness("build/vita-full/melee_vita")

# Keep OSReport silent; boot_component already turns HSD_Panic into an exception.
def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


if "OSReport" in arm.symbols:
    p = arm.symbols["OSReport"] & ~1
    arm.uc.hook_add(UC_HOOK_CODE, silent, begin=p, end=p)

boot_component(arm)
heap = arm.call("HSD_GetHeap")
assert heap >= 0

# HSD_ClassInfoHead is 40 bytes, then alloc/init/release/destroy/amnesia.
INIT_OFFSET = 44
RELEASE_OFFSET = 48
release_checks = [
    ("JObjInfoInit", "hsdJObj", "JObjRelease"),
    ("DObjInfoInit", "hsdDObj", "DObjRelease"),
    ("MObjInfoInit", "hsdMObj", "MObjRelease"),
    ("PObjInfoInit", "hsdPObj", "PObjRelease"),
    ("TObjInfoInit", "hsdTObj", "TObjRelease"),
]
for init_name, info_name, release_name in release_checks:
    arm.call(init_name)
    if info_name == "hsdJObj":
        actual_init = struct.unpack(
            "<I", arm.uc.mem_read(arm.symbols[info_name] + INIT_OFFSET, 4)
        )[0]
        expected_init = arm.symbols["JObjInit"]
        assert (actual_init & ~1) == (expected_init & ~1), (
            "hsdJObj.init",
            hex(actual_init),
            hex(expected_init),
        )
    actual = struct.unpack(
        "<I", arm.uc.mem_read(arm.symbols[info_name] + RELEASE_OFFSET, 4)
    )[0]
    expected = arm.symbols[release_name]
    assert (actual & ~1) == (expected & ~1), (
        info_name,
        hex(actual),
        hex(expected),
    )

name = arm.alloc(len(b"GmPause\0"))
arm.uc.mem_write(name, b"GmPause\0")


def one_cycle() -> int:
    src = arm.alloc(len(raw))
    arm.uc.mem_write(src, raw)
    arm.call("mv_boot_archive_prepare_raw", src, len(raw), name)
    archive = arm.alloc(0x44)
    assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0

    scene = src + 32 + scene_off
    models = struct.unpack("<I", arm.uc.mem_read(scene, 4))[0]
    model0 = struct.unpack("<I", arm.uc.mem_read(models, 4))[0]
    joint = struct.unpack("<I", arm.uc.mem_read(model0, 4))[0]
    assert models and model0 and joint

    root = arm.call("HSD_JObjLoadJoint", joint)
    assert root
    arm.call("HSD_JObjRemoveAll", root)
    return arm.call("OSCheckHeap", heap)


# First two cycles warm HSD fixed-size object pools. After that, repeatedly
# loading/removing the same retail graph must not consume additional OS heap.
one_cycle()
warm = one_cycle()
series = [one_cycle() for _ in range(ITERATIONS)]
assert min(series) == max(series) == warm, (warm, series)

print(
    f"PASS HSD release lifecycle: JObj/DObj/MObj/PObj/TObj release callbacks active; "
    f"GmPause load/remove heap stable for {ITERATIONS} post-warmup cycles at {warm} bytes free"
)
