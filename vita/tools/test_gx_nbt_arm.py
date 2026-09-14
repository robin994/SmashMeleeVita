#!/usr/bin/env python3
"""ARM regression for GX_VA_NBT aliasing and indexed NBT decode."""
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from arm_harness import ArmHarness

GX_VA_POS = 9
GX_VA_NBT = 25
GX_DIRECT = 1
GX_INDEX8 = 2
GX_VTXFMT0 = 0
GX_POS_XYZ = 1
GX_NRM_XYZ = 0
GX_F32 = 4
GX_TRIANGLES = 0x90

arm = ArmHarness("build/vita-full/melee_vita")
arm.call("mv_gx_capture_reset")

normal_bytes = struct.pack(">9f", *[float(i) for i in range(1, 10)])
normal_array = arm.alloc(len(normal_bytes))
arm.uc.mem_write(normal_array, normal_bytes)

arm.call("GXClearVtxDesc")
arm.call("GXSetVtxDesc", GX_VA_POS, GX_DIRECT)
arm.call("GXSetVtxAttrFmt", GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0)
arm.call("GXSetVtxDesc", GX_VA_NBT, GX_INDEX8)
# HSD shape paths can label NBT data with GX_NRM_XYZ. The Vita shim must
# still consume a full N+B+T entry while using the hardware NRM array slot.
arm.call("GXSetVtxAttrFmt", GX_VTXFMT0, GX_VA_NBT, GX_NRM_XYZ, GX_F32, 0)
arm.call("GXSetArray", GX_VA_NBT, normal_array, 36)

identity = struct.pack("<12f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0)
mtx = arm.alloc(len(identity))
arm.uc.mem_write(mtx, identity)
arm.call("GXLoadPosMtxImm", mtx, 0)

payload = bytearray([GX_TRIANGLES, 0, 3])
for pos in ((0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)):
    payload += struct.pack(">3f", *pos)
    payload += b"\x00"
dl = arm.alloc(len(payload))
arm.uc.mem_write(dl, bytes(payload))
arm.call("GXCallDisplayList", dl, len(payload))

stats_ptr = arm.alloc(64)
result = arm.call("mv_gx_capture_stats", stats_ptr)
stats = struct.unpack("<16I", bytes(arm.uc.mem_read(stats_ptr, 64)))
assert result == 0, f"capture result {result}, errors={stats[11]} line={stats[12]}"
assert stats[1:4] == (1, 3, 1), f"unexpected command/vertex/triangle stats {stats[1:4]}"
assert stats[11] == 0, f"capture errors={stats[11]} line={stats[12]}"

count_ptr = arm.alloc(4)
verts_ptr = arm.call("mv_gx_capture_vertices", count_ptr)
count = struct.unpack("<I", bytes(arm.uc.mem_read(count_ptr, 4)))[0]
assert count == 3
first = bytes(arm.uc.mem_read(verts_ptr, 68))
normal = struct.unpack_from("<9f", first, 12)
assert normal == tuple(float(i) for i in range(1, 10)), normal

print("PASS GX NBT ARM: GX_VA_NBT aliases NRM slot/array and GX_NRM_XYZ still decodes 9-scalar N+B+T payload without capture errors")
