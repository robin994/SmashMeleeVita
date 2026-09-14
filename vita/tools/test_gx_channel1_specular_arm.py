"""Verify the Vita GX bridge evaluates HSD's COLOR1/A1 specular channel."""
import argparse
import struct

from arm_component import boot_component
from arm_harness import ArmHarness
from unicorn.arm_const import (
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R2,
    UC_ARM_REG_R3,
    UC_ARM_REG_SP,
)


p = argparse.ArgumentParser()
p.add_argument("--elf", required=True)
args = p.parse_args()

arm = ArmHarness(args.elf)
boot_component(arm)


def call_chan_ctrl(chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn):
    """Call the 7-integer-argument AAPCS GXSetChanCtrl entrypoint."""
    uc = arm.uc
    sp = arm.stack + 65536 - 256
    uc.reg_write(UC_ARM_REG_SP, sp)
    uc.reg_write(UC_ARM_REG_LR, arm.stop)
    for reg, value in zip(
        (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3),
        (chan, enable, amb_src, mat_src),
    ):
        uc.reg_write(reg, value)
    uc.mem_write(sp, struct.pack("<III", light_mask, diff_fn, attn_fn))
    uc.emu_start(arm.symbols["GXSetChanCtrl"], arm.stop,
                 timeout=30000000, count=30000000)
    assert uc.reg_read(UC_ARM_REG_PC) == arm.stop


# HSD specular uses COLOR1/A1 with register ambient/material sources. GXColor
# is passed by value as four consecutive bytes, hence the little-endian words.
GX_COLOR1A1 = 5
GX_SRC_REG = 0
GX_DF_NONE = 0
GX_AF_SPEC = 0
GX_LIGHT0 = 1
arm.call("GXSetNumChans", 2)
arm.call("GXSetChanAmbColor", GX_COLOR1A1, 0xFF000000)  # 0,0,0,255
arm.call("GXSetChanMatColor", GX_COLOR1A1, 0xFFFFFFFF)
call_chan_ctrl(GX_COLOR1A1, 1, GX_SRC_REG, GX_SRC_REG,
               GX_LIGHT0, GX_DF_NONE, GX_AF_SPEC)

# NativeLight/GXLightObj ABI is 64 bytes: reserved[3], packed color,
# attenuation A[3]/K[3], position, direction. This synthetic light points and
# shines along +Z and yields a unit specular coefficient for a +Z normal.
light = arm.alloc(64)
payload = bytearray(64)
struct.pack_into("<I", payload, 12, 0xFFFFFFFF)
struct.pack_into("<3f", payload, 16, 0.0, 0.0, 1.0)  # A = x^2
struct.pack_into("<3f", payload, 28, 1.0, 0.0, 0.0)  # K = 1
struct.pack_into("<3f", payload, 40, 0.0, 0.0, 10.0)
struct.pack_into("<3f", payload, 52, 0.0, 0.0, 1.0)
arm.uc.mem_write(light, bytes(payload))
arm.call("GXLoadLightObjImm", light, GX_LIGHT0)

position = arm.alloc(12)
normal = arm.alloc(12)
flags = arm.alloc(4)
arm.uc.mem_write(position, struct.pack("<3f", 0.0, 0.0, 0.0))


def evaluate(n):
    arm.uc.mem_write(normal, struct.pack("<3f", *n))
    rgba = arm.call("mv_gx_channel1_eval", 0xFFFFFFFF, position, normal, flags)
    f = struct.unpack("<I", arm.uc.mem_read(flags, 4))[0]
    return rgba, f


front, front_flags = evaluate((0.0, 0.0, 1.0))
side, side_flags = evaluate((1.0, 0.0, 0.0))
back, back_flags = evaluate((0.0, 0.0, -1.0))

assert front == 0xFFFFFFFF, hex(front)
assert side == 0x000000FF, hex(side)
assert back == 0x000000FF, hex(back)
assert front_flags == side_flags == back_flags == 0x7, (
    hex(front_flags), hex(side_flags), hex(back_flags)
)

print(
    "PASS GX channel1 specular ARM: front=ffffffff side/back=000000ff "
    "with active+lit+normal flags"
)
