#!/usr/bin/env python3
"""Linked-ARM regression for source-derived synchronous SceneDesc sidecars."""

from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC

ASSETS = Path("orig/GALE01/files")
CASES = (
    # Core gameplay SceneDesc archives use specialized model collectors but
    # share the exact same camera/light/fog serialized HSD schema.
    ("IfAll.dat", "ScInfDmg_scene_data"),
    ("IfAll.usd", "ScInfDmg_scene_data"),
    ("GmPause.dat", "ScGamPause_scene_data"),
    ("GmPause.usd", "ScGamPause_scene_data"),
    ("IfCoGet.dat", "ScInfCgt_scene_data"),
    ("IfPrize.dat", "ScInfPrize_scene_data"),
    ("IfPrize.usd", "ScInfPrize_scene_data"),
    ("IfComSn.dat", "ScComSoon_scene_data"),
    ("IfComSn.usd", "ScComSoon_scene_data"),
    ("NtAppro.dat", "ScNtcApproach_scene_data"),
    ("NtAppro.usd", "ScNtcApproach_scene_data"),
    ("NtProge.dat", "ScNtcProgressive_scene_data"),
    ("NtMemAc.dat", "ScNtcCommon_scene_data"),
    ("NtMemAc.usd", "ScNtcCommon_scene_data"),
    ("NtMsgWin.dat", "ScNtcCommon_scene_data"),
    ("GmTou1p.dat", "ScGamTour_scene_data"),
    ("GmTou1p.usd", "ScGamTour_scene_data"),
    ("GmTou2p.dat", "ScGamTour_scene_data"),
    ("GmTou2p.usd", "ScGamTour_scene_data"),
    ("GmTou3p.dat", "ScGamTour_scene_data"),
    ("GmTou3p.usd", "ScGamTour_scene_data"),
    ("GmTou4p.dat", "ScGamTour_scene_data"),
    ("GmTou4p.usd", "ScGamTour_scene_data"),
    ("GmGover.dat", "ScGamRegGover_scene_data"),
    ("GmGoCoin.dat", "ScGamRegGover_scene_data"),
    ("GmGoAnim.dat", "ScGamRegGover_scene_data"),
    ("GmRegClr.dat", "ScGamRegClear_scene_data"),
    ("GmRegClr.usd", "ScGamRegClear_scene_data"),
    ("GmStRoll.dat", "ScGamRegStaffroll_scene_data"),
    ("IrNml.dat", "ScItrNormal_scene_data"),
    ("IrNml.usd", "ScItrNormal_scene_data"),
)


def public_offsets(raw: bytes):
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(">4I", raw, 4)
    public_off = 32 + data_size + reloc_count * 4
    strings_off = public_off + (public_count + extern_count) * 8
    out = {}
    for i in range(public_count):
        target, name_off = struct.unpack_from(">2I", raw, public_off + i * 8)
        start = strings_off + name_off
        end = raw.index(0, start)
        out[raw[start:end].decode("ascii")] = target
    return out


arm = ArmHarness("build/vita-full/melee_vita")
# This regression deliberately loads the two large IfAll variants plus the
# complete SceneDesc sidecar family in one process. ArmHarness models libc
# free() as arena-discard-at-process-exit, so extend only this test's scratch
# arena rather than dropping coverage when cumulative temporary allocations
# exceed the generic 96 MiB test budget.
_extra_scratch = 96 * 1024 * 1024
arm.uc.mem_map(arm.heap_end, _extra_scratch)
arm.heap_end += _extra_scratch


def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


if "OSReport" in arm.symbols:
    p = arm.symbols["OSReport"] & ~1
    arm.uc.hook_add(UC_HOOK_CODE, silent, begin=p, end=p)
boot_component(arm)
heap = arm.call("HSD_GetHeap")


def cstring(text: str):
    raw = text.encode("ascii") + b"\0"
    ptr = arm.alloc(len(raw))
    arm.uc.mem_write(ptr, raw)
    return ptr


def u32(addr):
    return struct.unpack("<I", arm.uc.mem_read(addr, 4))[0]


def u16(addr):
    return struct.unpack("<H", arm.uc.mem_read(addr, 2))[0]


def f32(addr):
    return struct.unpack("<f", arm.uc.mem_read(addr, 4))[0]


def vec3(addr):
    return struct.unpack("<3f", arm.uc.mem_read(addr, 12))


def close3(actual, expected, eps=1e-4):
    return all(abs(a - b) <= eps * max(1.0, abs(b))
               for a, b in zip(actual, expected))


loaded_models = 0
loaded_cameras = 0
loaded_lights = 0
loaded_fogs = 0
false_ptcl_sources = 0
ifcoget_baseline = None

for filename, root_name in CASES:
    print(f"SCENE_SIDECAR_CASE {filename} {root_name}", flush=True)
    raw = (ASSETS / filename).read_bytes()
    pub = public_offsets(raw)
    assert root_name in pub, (filename, root_name)

    # Prove the raw retail graph would falsely look like PTCL on little-endian
    # ARM before the source-derived SceneDesc converter runs.
    scene_off = pub[root_name]
    data_size, reloc_count = struct.unpack_from(">2I", raw, 4)
    reloc_off = 32 + data_size
    reloc = {struct.unpack_from(">I", raw, reloc_off + i * 4)[0] for i in range(reloc_count)}
    def be32(off): return struct.unpack_from(">I", raw, 32 + off)[0]
    def be16(off): return struct.unpack_from(">H", raw, 32 + off)[0]
    def bef32(off): return struct.unpack_from(">f", raw, 32 + off)[0]
    def rawptr(off): return be32(off) if off in reloc else 0

    # Capture semantic expectations from the untouched retail descriptor
    # graph. These are compared with both the pre-relocation host-native bytes
    # and the HSD runtime objects below; allocation alone is not correctness.
    expected_camera = None
    camera_table_off = rawptr(scene_off + 4)
    if camera_table_off:
        camera_desc_off = rawptr(camera_table_off)
        camera_anims_off = rawptr(camera_table_off + 4)
        if camera_desc_off:
            expected_camera = {
                "desc": camera_desc_off,
                "projection": be16(camera_desc_off + 6),
                "near": bef32(camera_desc_off + 0x28),
                "far": bef32(camera_desc_off + 0x2C),
                "anims": camera_anims_off,
            }

    expected_lights = []
    light_table_off = rawptr(scene_off + 8)
    if light_table_off:
        for i in range(64):
            light_list_off = rawptr(light_table_off + i * 4)
            if not light_list_off:
                break
            desc_off = rawptr(light_list_off)
            anims_off = rawptr(light_list_off + 4)
            while desc_off:
                flags = be16(desc_off + 8)
                pos_off = rawptr(desc_off + 16)
                interest_off = rawptr(desc_off + 20)
                expected_lights.append({
                    "desc": desc_off,
                    "flags": flags,
                    "type": flags & 3,
                    "position": ((bef32(pos_off + 4), bef32(pos_off + 8),
                                  bef32(pos_off + 12)) if pos_off else None),
                    "interest": ((bef32(interest_off + 4),
                                  bef32(interest_off + 8),
                                  bef32(interest_off + 12))
                                 if interest_off else None),
                    "anims": anims_off,
                })
                desc_off = rawptr(desc_off + 4)
        else:
            raise AssertionError((filename, "raw unterminated lights"))

    expected_fog = None
    fog_entry_off = rawptr(scene_off + 12)
    if fog_entry_off:
        fog_desc_off = rawptr(fog_entry_off)
        fog_anims_off = rawptr(fog_entry_off + 4)
        if fog_desc_off:
            expected_fog = {
                "desc": fog_desc_off,
                "type": be32(fog_desc_off),
                "start": bef32(fog_desc_off + 8),
                "end": bef32(fog_desc_off + 12),
                "anims": fog_anims_off,
            }
    if scene_off in reloc:
        table = be32(scene_off)
        for i in range(128):
            field = table + i * 4
            if field not in reloc:
                break
            model = be32(field)
            if model in reloc:
                joint = be32(model)
                if joint:
                    flags = be32(joint + 4)
                    le_flags = struct.unpack("<I", struct.pack(">I", flags))[0]
                    if not (flags & 0x20) and (le_flags & 0x20):
                        false_ptcl_sources += 1

    src = arm.alloc(len(raw))
    arm.uc.mem_write(src, raw)
    arm.call("mv_boot_archive_prepare_raw", src, len(raw), cstring(filename))
    data = src + 32

    # The raw/native boundary itself is part of the contract. If these values
    # are still PowerPC byte order, a permissive HSD loader can hide the bug
    # until unrelated game code interprets a flag (the v3.56 lbshadow crash).
    if expected_camera:
        desc = data + expected_camera["desc"]
        assert u16(desc + 6) == expected_camera["projection"], (
            filename, "camera projection endian", u16(desc + 6), expected_camera)
        assert abs(f32(desc + 0x28) - expected_camera["near"]) < 1e-4
        assert abs(f32(desc + 0x2C) - expected_camera["far"]) < 1e-3
    for expected in expected_lights:
        assert u16(data + expected["desc"] + 8) == expected["flags"], (
            filename, "light flags endian", hex(expected["desc"]),
            hex(u16(data + expected["desc"] + 8)), hex(expected["flags"]))
    if expected_fog:
        desc = data + expected_fog["desc"]
        assert u32(desc) == expected_fog["type"], (filename, "fog type endian")
        assert abs(f32(desc + 8) - expected_fog["start"]) < 1e-4
        assert abs(f32(desc + 12) - expected_fog["end"]) < 1e-4

    archive = arm.alloc(0x44)
    assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0, filename

    scene = src + 32 + scene_off
    models = u32(scene)
    if models:
        for i in range(128):
            model = u32(models + i * 4)
            if not model:
                break
            joint = u32(model)
            if joint:
                root = arm.call("HSD_JObjLoadJoint", joint)
                assert root, (filename, root_name, i, hex(joint))
                arm.call("HSD_JObjRemoveAll", root)
                loaded_models += 1
        else:
            raise AssertionError((filename, "unterminated models"))

    # Exercise the non-model SceneDesc branches too. These calls intentionally
    # fail the regression if camera/light/fog scalars are still big-endian.
    cameras = u32(scene + 4)
    if cameras:
        # The synchronous SceneDesc users in the source-derived registry read
        # cameras[0].  Camera arrays are not uniformly NULL-pair terminated;
        # some retail files intentionally carry animation data with desc=NULL.
        desc = u32(cameras)
        if desc:
            cobj = arm.call("HSD_CObjLoadDesc", desc)
            assert cobj, (filename, "camera", 0)
            assert expected_camera is not None
            assert arm.uc.mem_read(cobj + 0x50, 1)[0] == expected_camera["projection"], (
                filename, "runtime camera projection")
            assert abs(f32(cobj + 0x38) - expected_camera["near"]) < 1e-4
            assert abs(f32(cobj + 0x3C) - expected_camera["far"]) < 1e-3
            anims = u32(cameras + 4)
            if anims:
                canim = u32(anims)
                if canim:
                    arm.call("HSD_CObjAddAnim", cobj, canim)
                    arm.call("HSD_CObjReqAnim", cobj, 0)
                    arm.call("HSD_CObjAnim", cobj)
            loaded_cameras += 1

    lights = u32(scene + 8)
    if lights:
        # Use the same Melee helper as the real SceneDesc path. This exercises
        # descriptor chains plus HSD_LightAnim/HSD_WObjAnim loading.
        lobj_head = arm.call("lb_80011AC4", lights)
        assert lobj_head, (filename, "light-list load")
        runtime = []
        cur = lobj_head
        position_out = arm.alloc(12)
        interest_out = arm.alloc(12)
        while cur:
            flags = arm.call("HSD_LObjGetFlags", cur) & 0xFFFF
            has_pos = arm.call("HSD_LObjGetPosition", cur, position_out)
            pos = vec3(position_out) if has_pos else None
            has_interest = arm.call("HSD_LObjGetInterest", cur, interest_out)
            interest = vec3(interest_out) if has_interest else None
            runtime.append((cur, flags, pos, interest))
            cur = u32(cur + 0x0C)
            assert len(runtime) <= 64, (filename, "runtime light cycle")

        assert len(runtime) == len(expected_lights), (
            filename, "light count", len(runtime), len(expected_lights))
        for index, ((_, flags, pos, interest), expected) in enumerate(
                zip(runtime, expected_lights)):
            assert flags == expected["flags"], (
                filename, "runtime light flags", index, hex(flags),
                hex(expected["flags"]))
            assert (flags & 3) == expected["type"]
            assert (pos is not None) == (expected["position"] is not None), (
                filename, "light position presence", index, flags)
            if pos is not None:
                assert close3(pos, expected["position"]), (
                    filename, "light position", index, pos, expected["position"])
            assert (interest is not None) == (expected["interest"] is not None), (
                filename, "light interest presence", index, flags)
            if interest is not None:
                assert close3(interest, expected["interest"]), (
                    filename, "light interest", index, interest,
                    expected["interest"])
            loaded_lights += 1

        if filename == "IfCoGet.dat":
            # Reproduce lbShadow_8000F38C's light choice offline. Retail must
            # fall back to the point light (0x000E), which owns a position;
            # the broken BE path chose ambient 0x0400 and asserted at line 386.
            selected = None
            fallback = None
            for entry in runtime:
                flags = entry[1]
                if flags & 3:
                    fallback = entry
                if flags & 0x400:
                    selected = entry
                    break
            if selected is None:
                selected = fallback
            assert selected is not None
            assert selected[1] == 0x000E, ("IfCoGet shadow light", selected)
            assert selected[2] is not None, ("IfCoGet shadow position", selected)

        arm.call("HSD_LObjRemoveAll", lobj_head)

    fogs = u32(scene + 12)
    if fogs:
        # Same rule as cameras: registered synchronous users consume fogs[0].
        desc = u32(fogs)
        if desc:
            fog = arm.call("HSD_FogLoadDesc", desc)
            assert fog, (filename, "fog", 0)
            assert expected_fog is not None
            assert u32(fog + 0x08) == expected_fog["type"], (
                filename, "runtime fog type")
            assert abs(f32(fog + 0x10) - expected_fog["start"]) < 1e-4
            assert abs(f32(fog + 0x14) - expected_fog["end"]) < 1e-4
            anims = u32(fogs + 4)
            if anims:
                fanim = u32(anims)
                if fanim:
                    aobjdesc = u32(fanim)
                    if aobjdesc:
                        arm.call("HSD_Fog_8037DE7C", fog, aobjdesc)
                        arm.call("HSD_FogReqAnim", fog, 0)
                        arm.call("HSD_FogInterpretAnim", fog)
            loaded_fogs += 1

    if filename == "IfCoGet.dat":
        # The exact current hardware sidecar must be stable under repeated
        # model load/remove after fixed-size pool warm-up.
        model = u32(models)
        joint = u32(model)
        for _ in range(2):
            root = arm.call("HSD_JObjLoadJoint", joint)
            arm.call("HSD_JObjRemoveAll", root)
        ifcoget_baseline = arm.call("OSCheckHeap", heap)
        series = []
        for _ in range(24):
            root = arm.call("HSD_JObjLoadJoint", joint)
            arm.call("HSD_JObjRemoveAll", root)
            series.append(arm.call("OSCheckHeap", heap))
        assert min(series) == max(series) == ifcoget_baseline, (ifcoget_baseline, series)

assert false_ptcl_sources > 0
assert ifcoget_baseline is not None
print(
    f"PASS SceneDesc sidecars: cases={len(CASES)} models={loaded_models} "
    f"cameras={loaded_cameras} lights={loaded_lights} fogs={loaded_fogs} "
    f"false_ptcl_sources={false_ptcl_sources}; semantic camera/light/fog + "
    f"IfCoGet shadow-light regression PASS; IfCoGet heap stable for 24 cycles "
    f"at {ifcoget_baseline} bytes free"
)
