#!/usr/bin/env python3
"""Exercise the compiled ARM SIS state stack with GameCube-order payloads."""
import argparse
import struct
from pathlib import Path

from arm_harness import ArmHarness


ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--elf", type=Path,
                    default=ROOT / "build/vita-full/melee_vita")
args = parser.parse_args()
arm = ArmHarness(str(args.elf))

TEXT_SIZE = 0xA4
STRING_BUFFER = 0x68
STRING_CAPACITY = 0x6E
SPACING = 0x78
SCALE = 0x80


def make_text():
    text = arm.alloc(TEXT_SIZE)
    buf = arm.alloc(0x40)
    arm.uc.mem_write(text, bytes(TEXT_SIZE))
    arm.uc.mem_write(buf, bytes(0x40))
    arm.uc.mem_write(text + STRING_BUFFER, struct.pack('<I', buf))
    arm.uc.mem_write(text + STRING_CAPACITY, struct.pack('<H', 0x40))
    return text, buf


def read_vec2(text, offset):
    return struct.unpack('<2f', arm.uc.mem_read(text + offset, 8))


text, buf = make_text()
arm.uc.mem_write(text + SPACING, struct.pack('<2f', 1.25, -2.5))
arm.call('HSD_SisLib_803A7684', text, 0, 1)
actual = bytes(arm.uc.mem_read(buf, 5))
assert actual == b'\x01\x40\xfd\x80\x01', actual.hex()
arm.uc.mem_write(text + SPACING, struct.pack('<2f', 0.0, 0.0))
arm.call('HSD_SisLib_803A7F0C', text, 1)
x, y = read_vec2(text, SPACING)
assert abs(x - 1.25) < 1e-6 and abs(y + 2.5) < 1e-6

text, buf = make_text()
arm.uc.mem_write(text + SCALE, struct.pack('<2f', 1.5, 0.75))
arm.call('HSD_SisLib_803A7684', text, 0, 3)
actual = bytes(arm.uc.mem_read(buf, 5))
assert actual == b'\x01\x80\x00\xc0\x03', actual.hex()
arm.uc.mem_write(text + SCALE, struct.pack('<2f', 0.0, 0.0))
arm.call('HSD_SisLib_803A7F0C', text, 3)
x, y = read_vec2(text, SCALE)
assert abs(x - 1.5) < 1e-6 and abs(y - 0.75) < 1e-6

text, buf = make_text()
cursor = 0x06123450
arm.call('HSD_SisLib_803A7684', text, cursor, 5)
actual = bytes(arm.uc.mem_read(buf, 5))
assert actual == b'\x06\x12\x34\x50\x05', actual.hex()
assert arm.call('HSD_SisLib_803A7F0C', text, 5) == cursor

print('PASS SIS ARM stack: signed/unsigned 8.8 state and return pointer round-trip')
