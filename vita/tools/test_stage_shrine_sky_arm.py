#!/usr/bin/env python3
"""Regression for Hyrule Temple's background shell and GX winding on Vita."""

import argparse
import math
import struct
import sys
from pathlib import Path

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R2

sys.path.insert(0, str(Path(__file__).resolve().parent))
from arm_component import boot_component
from arm_harness import ArmHarness

CMD_SIZE = 496
VERT_SIZE = 88
GX_TRIANGLES = 0x90
GX_TRIANGLESTRIP = 0x98
GX_TRIANGLEFAN = 0xA0
GX_QUADS = 0x80
GX_CULL_BACK = 2


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", default="build/vita-full/melee_vita")
    parser.add_argument("--asset", default="orig/GALE01/files/GrSh.dat")
    args = parser.parse_args()

    raw = Path(args.asset).read_bytes()
    arm = ArmHarness(args.elf)

    def cstring(text: str) -> int:
        ptr = arm.alloc(len(text) + 1)
        arm.uc.mem_write(ptr, text.encode() + b"\0")
        return ptr

    def u32(addr: int) -> int:
        return struct.unpack("<I", arm.uc.mem_read(addr, 4))[0]

    def silent(mu, _addr, _size, _data):
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def panic(mu, _addr, _size, _data):
        msg = arm.string(mu.reg_read(UC_ARM_REG_R2)).decode(errors="replace")
        raise RuntimeError(f"HSD_Panic: {msg}")

    for name, hook in (("OSReport", silent), ("HSD_Panic", panic)):
        addr = arm.symbols[name] & ~1
        arm.uc.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)

    boot_component(arm)

    def call_long(name: str, *call_args: int) -> int:
        try:
            return arm.call(name, *call_args)
        except AssertionError as exc:
            if str(exc) != f"{name}: did not return":
                raise
        for _ in range(40):
            pc = arm.uc.reg_read(UC_ARM_REG_PC)
            thumb = bool(arm.uc.reg_read(UC_ARM_REG_CPSR) & 32)
            arm.uc.emu_start(pc | thumb, arm.stop, timeout=30_000_000, count=100_000_000)
            if arm.uc.reg_read(UC_ARM_REG_PC) == arm.stop:
                return 0
        raise RuntimeError(f"{name}: timeout")

    source = arm.alloc(len(raw))
    arm.uc.mem_write(source, raw)
    call_long("mv_stage_archive_prepare_raw", source, len(raw), cstring("GrSh.dat"))

    archive = arm.alloc(0x44)
    if arm.call("HSD_ArchiveParse", archive, source, len(raw)) != 0:
        raise SystemExit("FAIL GrSh sky: archive parse")
    map_head = arm.call("HSD_ArchiveGetPublicAddress", archive, cstring("map_head"))
    if not map_head:
        raise SystemExit("FAIL GrSh sky: map_head")
    arm.call("mv_stage_archive_prepare", archive, map_head, 0, 0, 0)

    entries = u32(map_head + 8)
    if u32(map_head + 0x0C) != 3:
        raise SystemExit("FAIL GrSh sky: expected three map entries")
    entry = entries + 0x34  # map 1 is the animated sky/background shell.

    arm.call("HSD_IDInitAllocData")
    arm.call("HSD_IDSetup")
    root = arm.call("HSD_JObjLoadJoint", u32(entry))
    if not root:
        raise SystemExit("FAIL GrSh sky: JObj load")

    anim_table = u32(entry + 4)
    matanim_table = u32(entry + 8)
    anim = u32(anim_table) if anim_table else 0
    matanim = u32(matanim_table) if matanim_table else 0
    arm.call("HSD_JObjAddAnimAll", root, anim, matanim, 0)
    arm.call("HSD_JObjReqAnimAll", root, 0)
    arm.call("HSD_JObjAnimAll", root)

    stats = arm.alloc(256)
    if arm.call("mv_hsd_gx_capture_runtime", root, 1, 0, stats) != 0:
        raise SystemExit("FAIL GrSh sky: capture")
    count_ptr = arm.alloc(4)
    commands = arm.call("mv_gx_capture_commands", count_ptr)
    count = u32(count_ptr)
    vertex_count_ptr = arm.alloc(4)
    vertices = arm.call("mv_gx_capture_vertices", vertex_count_ptr)

    if count != 26:
        raise SystemExit(f"FAIL GrSh sky: expected 26 commands, got {count}")

    transformed = []
    command_data = []
    for ci in range(count):
        command = commands + ci * CMD_SIZE
        first = u32(command)
        nverts = u32(command + 4)
        ntris = u32(command + 8)
        cull = u32(command + 20)
        primitive = arm.uc.mem_read(command + 24, 1)[0]
        if cull != GX_CULL_BACK:
            raise SystemExit(f"FAIL GrSh sky: command {ci} cull={cull}, expected back")
        matrix = struct.unpack("<12f", arm.uc.mem_read(command + 28, 48))
        points = []
        for vi in range(nverts):
            x, y, z = struct.unpack("<3f", arm.uc.mem_read(vertices + (first + vi) * VERT_SIZE, 12))
            point = (
                matrix[0] * x + matrix[1] * y + matrix[2] * z + matrix[3],
                matrix[4] * x + matrix[5] * y + matrix[6] * z + matrix[7],
                matrix[8] * x + matrix[9] * y + matrix[10] * z + matrix[11],
            )
            if not all(math.isfinite(v) for v in point):
                raise SystemExit(f"FAIL GrSh sky: non-finite vertex command={ci}")
            points.append(point)
            transformed.append(point)
        command_data.append((primitive, ntris, points))

    mins = [min(p[k] for p in transformed) for k in range(3)]
    maxs = [max(p[k] for p in transformed) for k in range(3)]
    center = [(mins[k] + maxs[k]) * 0.5 for k in range(3)]

    outward = inward = degenerate = 0
    for primitive, ntris, points in command_data:
        for tri in range(ntris):
            if primitive == GX_TRIANGLES:
                ids = [tri * 3, tri * 3 + 1, tri * 3 + 2]
            elif primitive == GX_QUADS:
                base = (tri // 2) * 4
                ids = [base, base + 2, base + 3] if tri & 1 else [base, base + 1, base + 2]
            elif primitive == GX_TRIANGLESTRIP:
                ids = [tri, tri + 1, tri + 2]
                if tri & 1:
                    ids[0], ids[1] = ids[1], ids[0]
            elif primitive == GX_TRIANGLEFAN:
                ids = [0, tri + 1, tri + 2]
            else:
                continue

            a, b, c = (points[i] for i in ids)
            u = [b[k] - a[k] for k in range(3)]
            v = [c[k] - a[k] for k in range(3)]
            cross = [
                u[1] * v[2] - u[2] * v[1],
                u[2] * v[0] - u[0] * v[2],
                u[0] * v[1] - u[1] * v[0],
            ]
            area = math.sqrt(sum(x * x for x in cross))
            if area < 1e-5:
                degenerate += 1
                continue
            radial = [(a[k] + b[k] + c[k]) / 3.0 - center[k] for k in range(3)]
            dot = sum(cross[k] * radial[k] for k in range(3))
            if dot > 0:
                outward += 1
            elif dot < 0:
                inward += 1
            else:
                degenerate += 1

    if outward != 224 or inward != 0:
        raise SystemExit(
            f"FAIL GrSh sky winding: outward={outward} inward={inward} degenerate={degenerate}"
        )

    print(
        "PASS GrSh sky: commands=26 cull=GX_CULL_BACK "
        f"outward={outward} inward={inward} degenerate={degenerate}; "
        "retail shell preserves clockwise GX front-face semantics"
    )


if __name__ == "__main__":
    main()
