#!/usr/bin/env python3
"""ARM regression for every native MnSlMap.usd SSS model/animation set."""

import argparse
from pathlib import Path
import struct

from arm_component import boot_component
from arm_harness import ArmHarness


ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--elf", type=Path,
                    default=ROOT / "build/vita-full/melee_vita")
parser.add_argument("--asset", type=Path,
                    default=ROOT / "orig/GALE01/files/MnSlMap.usd")
args = parser.parse_args()
ELF = args.elf
ASSET = args.asset


def public_offset(raw: bytes, wanted: bytes) -> int:
    data_size, reloc_count, public_count = struct.unpack_from(">III", raw, 4)
    public_base = 0x20 + data_size + reloc_count * 4
    strings_base = public_base + public_count * 8
    for index in range(public_count):
        target, name_offset = struct.unpack_from(">II", raw,
                                                  public_base + index * 8)
        end = raw.index(0, strings_base + name_offset)
        if raw[strings_base + name_offset:end] == wanted:
            return target
    raise AssertionError(f"missing public symbol {wanted!r}")


arm = ArmHarness(str(ELF))
boot_component(arm)
raw = ASSET.read_bytes()
source = arm.alloc(len(raw))
arm.uc.mem_write(source, raw)
view = arm.alloc(128)
assert arm.call("mv_dat_open", view, source, len(raw)) == 0

table = public_offset(raw, b"MnSelectStageDataTable")
data_size = struct.unpack_from(">I", raw, 4)[0]
model_graphs = []
shape_roots = []
set10_shapeanim = None
for index in range(12):
    base = table + (0x10 + index * 0x10 if index < 11 else 0xC0)
    model, anim, matanim, shapeanim = struct.unpack_from(">IIII", raw,
                                                        0x20 + base)
    assert model < data_size
    graph = arm.alloc(72)
    assert arm.call("mv_hsd_native_build_at", view, model, graph) == 0
    root = struct.unpack("<I", arm.uc.mem_read(graph, 4))[0]
    assert root
    runtime_root = arm.call("HSD_JObjLoadJoint", root)
    assert runtime_root
    arm.call("HSD_JObjRemoveAll", runtime_root)
    model_graphs.append(graph)
    if index == 10:
        shape_roots.append(root)

    for offset, builder, releaser in (
        (anim, "mv_native_anim_build_at", "mv_native_anim_free"),
        (matanim, "mv_native_matanim_build_at", "mv_native_matanim_free"),
        (shapeanim, "mv_native_shapeanim_build_at", "mv_native_shapeanim_free"),
    ):
        if not offset:
            continue
        assert offset < data_size
        native = arm.alloc(64)
        assert arm.call(builder, view, offset, native) == 0
        native_root = struct.unpack("<I", arm.uc.mem_read(native, 4))[0]
        assert native_root
        if index == 10 and builder == "mv_native_shapeanim_build_at":
            set10_shapeanim = native
        else:
            arm.call(releaser, native)

assert len(shape_roots) == 1
assert set10_shapeanim is not None
root = shape_roots[0]

pending = [root]
seen_joints = set()
seen_dobjs = set()
shape_descs = []
while pending:
    joint = pending.pop()
    if not joint or joint in seen_joints:
        continue
    seen_joints.add(joint)
    child, next_joint, dobj = struct.unpack("<III",
                                            arm.uc.mem_read(joint + 8, 12))
    pending.extend((child, next_joint))
    while dobj and dobj not in seen_dobjs:
        seen_dobjs.add(dobj)
        next_dobj, pobj = struct.unpack("<I4xI",
                                        arm.uc.mem_read(dobj + 4, 12))
        while pobj:
            next_pobj, flags, shape = struct.unpack(
                "<I4xH6xI", arm.uc.mem_read(pobj + 4, 20))
            if flags & 0x3000 == 0x1000:
                shape_descs.append(shape)
            pobj = next_pobj
        dobj = next_dobj

assert shape_descs
for shape in shape_descs:
    flags, count, vertex_count, vertex_desc, vertex_lists, normal_count, normal_desc, normal_lists = struct.unpack(
        "<HHIIIIII", arm.uc.mem_read(shape, 28))
    assert count > 0 and vertex_count >= 0 and normal_count >= 0
    if vertex_count:
        assert vertex_desc and vertex_lists
        list_count = count + 1 if flags & 2 else count
        assert all(struct.unpack("<I", arm.uc.mem_read(vertex_lists + i * 4, 4))[0]
                   for i in range(list_count))
    if normal_count:
        assert normal_desc and normal_lists

runtime_root = arm.call("HSD_JObjLoadJoint", root)
shapeanim_root = struct.unpack("<I", arm.uc.mem_read(set10_shapeanim, 4))[0]
assert runtime_root and shapeanim_root
arm.call("HSD_JObjAddAnimAll", runtime_root, 0, 0, shapeanim_root)
arm.call("HSD_JObjReqAnimAll", runtime_root, 0)
arm.call("HSD_JObjAnimAll", runtime_root)
capture = arm.alloc(48)
assert arm.call("mv_hsd_gx_capture_runtime", runtime_root, 1, 0, capture) == 0
display_lists, commands, vertices, triangles = struct.unpack(
    "<IIII", arm.uc.mem_read(capture, 16))
assert display_lists > 0 and commands > 0 and vertices > 0 and triangles > 0, (
    display_lists, commands, vertices, triangles)
arm.call("HSD_JObjRemoveAll", runtime_root)
arm.call("mv_native_shapeanim_free", set10_shapeanim)

for graph in model_graphs:
    arm.call("mv_hsd_native_free", graph)
arm.call("mv_dat_close", view)
print(f"PASS MnSlMap: 12 model sets and every present JObj/Mat/Shape animation; "
      f"set 10 has {len(shape_descs)} native ShapeSet descriptor(s).")
