#!/usr/bin/env python3
"""Verify that the linked Vita default SIS atlas exactly matches main.dol."""
import argparse
import hashlib
import struct
from pathlib import Path
from arm_harness import ArmHarness

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--dol", type=Path,
                    default=ROOT / "orig/GALE01/sys/main.dol")
parser.add_argument("--elf", type=Path,
                    default=ROOT / "build/vita-full/melee_vita")
args = parser.parse_args()
DOL = args.dol
ELF = args.elf
ADDR = 0x8040CD40
SIZE = 287 * 512


def dol_slice(dol: bytes, address: int, size: int) -> bytes:
    sections = []
    for count, off_off, addr_off, size_off in ((7, 0x00, 0x48, 0x90), (11, 0x1C, 0x64, 0xAC)):
        offsets = struct.unpack_from(f">{count}I", dol, off_off)
        addrs = struct.unpack_from(f">{count}I", dol, addr_off)
        sizes = struct.unpack_from(f">{count}I", dol, size_off)
        sections.extend(zip(offsets, addrs, sizes))
    for offset, base, length in sections:
        if length and base <= address and address + size <= base + length:
            start = offset + address - base
            return dol[start:start + size]
    raise AssertionError("atlas address outside DOL")


dol = DOL.read_bytes()
expected = dol_slice(dol, ADDR, SIZE)
arm = ArmHarness(str(ELF))
symbol = arm.symbols["HSD_SisLib_FontAtlas"]
actual = bytes(arm.uc.mem_read(symbol, SIZE))
assert actual == expected
assert any(actual)
assert arm.call("mv_gx_alpha_compare_vitagl_supported", 4, 0, 1, 4, 0) == 1
assert arm.call("mv_gx_alpha_compare_vitagl_supported", 4, 0, 1, 0, 0) == 1
assert arm.call("mv_gx_alpha_compare_vitagl_supported", 4, 1, 1, 4, 0) == 0
print("PASS SIS font atlas:", SIZE, "bytes", hashlib.sha256(actual).hexdigest())
