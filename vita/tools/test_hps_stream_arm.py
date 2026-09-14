#!/usr/bin/env python3
"""Regression for little-endian Vita normalization of every HPS block header."""

import struct

from arm_component import boot_component
from arm_harness import ArmHarness

arm = ArmHarness("build/vita-full/melee_vita")
boot_component(arm)

def check(chunk, end, nxt, halves):
    raw = struct.pack(">III10H", chunk, end, nxt, *halves)
    ptr = arm.alloc(32)
    arm.uc.mem_write(ptr, raw)
    arm.call("HSD_SynthVitaNormalizeStreamBlockHeader", ptr)
    got = arm.uc.mem_read(ptr, 32)
    out_chunk, out_end, out_next, *out_halves = struct.unpack("<III10H", got)
    assert (out_chunk, out_end, out_next) == (chunk, end, nxt), (
        hex(out_chunk), hex(out_end), hex(out_next))
    assert out_halves == halves

# Exact hardware failure: these bytes were interpreted as 0xC0000200 in v3.76.
check(0x00010000, 0x0000FFFF, 0x000200C0,
      [0x1234, 0x5678, 0x9ABC, 0xDEF0, 1, 2, 3, 4, 5, 6])
# End-of-stream sentinel must survive normalization too.
check(0x00008000, 0x00007FFF, 0xFFFFFFFF, list(range(10)))
print("PASS HPS stream block endian: 0x000200C0 remains 0x000200C0 and EOF sentinel remains 0xFFFFFFFF")
