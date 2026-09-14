#!/usr/bin/env python3
"""Linked-ARM regression for stage ALDYakuAll item command nativeization."""

from pathlib import Path
import struct

from arm_harness import ArmHarness
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R2

ROOT = Path("orig/GALE01/files")
CASES = ("GrCs.dat", "GrGr.dat")


def parse_dat(raw: bytes):
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(">4I", raw, 4)
    reloc_base = 32 + data_size
    public_base = reloc_base + reloc_count * 4
    strings_base = public_base + (public_count + extern_count) * 8
    reloc = {
        struct.unpack_from(">I", raw, reloc_base + i * 4)[0]
        for i in range(reloc_count)
    }
    publics = {}
    for i in range(public_count):
        target, name_off = struct.unpack_from(">2I", raw, public_base + i * 8)
        start = strings_base + name_off
        end = raw.index(0, start)
        publics[raw[start:end].decode("ascii")] = target
    return data_size, reloc, publics


def repack(word: int, widths):
    shift = 32
    arm = 0
    out = 0
    for width in widths:
        shift -= width
        out |= ((word >> shift) & ((1 << width) - 1)) << arm
        arm += width
    assert shift == 0 and arm == 32
    return out


def native_u32(blob: bytes, data_off: int):
    return struct.unpack_from("<I", blob, 32 + data_off)[0]


arm = ArmHarness("build/vita-full/melee_vita")


def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


def panic(machine, _address, _size, _data):
    raise AssertionError(arm.string(machine.reg_read(UC_ARM_REG_R2)).decode(errors="replace"))


for name, hook in (("OSReport", silent), ("HSD_Panic", panic)):
    addr = arm.symbols[name] & ~1
    arm.uc.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)

histogram = {}
script_total = 0
for filename in CASES:
    raw = (ROOT / filename).read_bytes()
    _data_size, reloc, publics = parse_dat(raw)
    table = publics["ALDYakuAll"]
    assert struct.unpack_from(">I", raw, 32 + table)[0] == 0

    src = arm.alloc(len(raw))
    arm.uc.mem_write(src, raw)
    name = arm.alloc(len(filename) + 1)
    arm.uc.mem_write(name, filename.encode("ascii") + b"\0")
    arm.call("mv_stage_archive_prepare_raw", src, len(raw), name)
    after = bytes(arm.uc.mem_read(src, len(raw)))

    for state in range(1, 8):
        field = table + state * 4
        script = struct.unpack_from(">I", raw, 32 + field)[0]
        if script == 0:
            break
        assert field in reloc, (filename, state, hex(field))
        script_total += 1
        word = 0
        done = False
        while not done and word < 64:
            off = script + word * 4
            be = struct.unpack_from(">I", raw, 32 + off)[0]
            opcode = be >> 26
            histogram[opcode] = histogram.get(opcode, 0) + 1

            if opcode in (0, 1, 3, 4, 15):
                expected = repack(be, (6, 26))
                assert native_u32(after, off) == expected, (
                    filename, state, word, opcode, hex(native_u32(after, off)), hex(expected)
                )
                word += 1
                done = opcode == 0
            elif opcode == 7:
                expected = repack(be, (6, 26))
                assert native_u32(after, off) == expected
                assert off + 4 in reloc
                assert after[32 + off + 4:32 + off + 8] == raw[32 + off + 4:32 + off + 8]
                word += 2
                done = True
            elif opcode == 10:
                expected = repack(be, (6, 10, 16))
                native = native_u32(after, off)
                assert native == expected
                assert (native & 0x3F) == 10
                assert ((native >> 6) & 0x3FF) == ((be >> 16) & 0x3FF)
                for payload in range(1, 5):
                    p_off = off + payload * 4
                    p_be = struct.unpack_from(">I", raw, 32 + p_off)[0]
                    assert native_u32(after, p_off) == repack(p_be, (16, 16))
                word += 5
            elif opcode == 11:
                layouts = (
                    (6, 3, 3, 7, 13),
                    (16, 16),
                    (16, 16),
                    (9, 9, 9, 1, 1, 1, 1, 1),
                    (9, 5, 1, 8, 3, 4, 1, 1),
                )
                for payload, widths in enumerate(layouts):
                    p_off = off + payload * 4
                    p_be = struct.unpack_from(">I", raw, 32 + p_off)[0]
                    assert native_u32(after, p_off) == repack(p_be, widths), (
                        filename, state, word, payload
                    )
                assert after[32 + off + 20:32 + off + 24] == raw[32 + off + 20:32 + off + 24]
                word += 6
            elif opcode == 23:
                expected = repack(be, (6, 13, 13))
                assert native_u32(after, off) == expected
                word += 1
            else:
                raise AssertionError(
                    f"{filename}: unsupported retail Yaku opcode {opcode} state={state} word={word}"
                )
        assert done, (filename, state, word)

assert script_total == 5, script_total
assert set(histogram) == {0, 1, 3, 4, 7, 10, 11, 15, 23}, histogram
print(
    "PASS stage ALDYakuAll scripts: "
    f"cases={len(CASES)} scripts={script_total} "
    + " ".join(f"op{op}={histogram[op]}" for op in sorted(histogram))
    + "; GrCs crash script and GrGr extended grammar nativeize with goto relocation preserved"
)
