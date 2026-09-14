#!/usr/bin/env python3
"""ARM regression for PPC->ARM fighter subaction command nativeization."""
from pathlib import Path
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC

ASSET = Path("orig/GALE01/files/PlFx.dat")
ROOT_NAME = "ftDataFox"
COMMAND_WORDS = [
    1,1,1,1,1,2,1,2,1,1,
    5,5,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,3,1,1,1,7,4,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,3,3,2,1,4,
]


def parse_dat(raw: bytes):
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(">4I", raw, 4)
    reloc_base = 32 + data_size
    public_base = reloc_base + reloc_count * 4
    strings_base = public_base + (public_count + extern_count) * 8
    relocs = {
        struct.unpack_from(">I", raw, reloc_base + i * 4)[0]
        for i in range(reloc_count)
    }
    publics = {}
    for i in range(public_count):
        target, name_off = struct.unpack_from(">2I", raw, public_base + i * 8)
        start = strings_base + name_off
        end = raw.index(0, start)
        publics[raw[start:end].decode("ascii")] = target
    return data_size, relocs, publics


def be32(raw: bytes, off: int) -> int:
    return struct.unpack_from(">I", raw, 32 + off)[0]


def collect_commands(raw: bytes, data_size: int, relocs: set[int], roots: list[int]):
    commands = {}
    pointer_words = {}
    seen = set()

    def walk(off: int, depth: int = 0):
        assert depth <= 64
        while off not in seen:
            assert 0 <= off <= data_size - 4
            assert off not in relocs, hex(off)
            seen.add(off)
            word = be32(raw, off)
            opcode = word >> 26
            assert opcode < len(COMMAND_WORDS), (hex(off), opcode, hex(word))
            commands[off] = opcode
            if opcode in (0, 6):
                return
            if opcode in (5, 7):
                ptr_off = off + 4
                raw_ptr = raw[32 + ptr_off:32 + ptr_off + 4]
                pointer_words[ptr_off] = raw_ptr
                if ptr_off in relocs:
                    walk(be32(raw, ptr_off), depth + 1)
                else:
                    assert opcode == 7 and be32(raw, ptr_off) == 0, (hex(ptr_off), opcode)
                if opcode == 7:
                    return
            off += COMMAND_WORDS[opcode] * 4

    for root in roots:
        walk(root)
    return commands, pointer_words


raw = ASSET.read_bytes()
data_size, relocs, publics = parse_dat(raw)
root = publics[ROOT_NAME]
main = be32(raw, root + 0x0C)
demo = be32(raw, root + 0x14)
assert root + 0x0C in relocs and root + 0x14 in relocs
assert demo > main and (demo - main) % 0x18 == 0
motion_count = (demo - main) // 0x18
script_roots = []
landing_bad = []
for i in range(motion_count):
    rec = main + i * 0x18
    script_field = rec + 0x0C
    if script_field in relocs:
        script = be32(raw, script_field)
        script_roots.append(script)
        if be32(raw, script) == 0xDC000404:
            landing_bad.append(script)

assert landing_bad, "Fox corpus lost the DC000404 Landing regression word"
commands, pointer_words = collect_commands(raw, data_size, relocs, script_roots)
assert any(off in commands and commands[off] == 55 for off in landing_bad)
# This is exactly the physical-v3.85 failure: raw BE bytes read as a little-endian
# ARM C bitfield give low-six opcode 28 (SetHurtState).
assert struct.unpack("<I", struct.pack(">I", 0xDC000404))[0] & 0x3F == 28

arm = ArmHarness("build/vita-full/melee_vita")


def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


if "OSReport" in arm.symbols:
    p = arm.symbols["OSReport"] & ~1
    arm.uc.hook_add(UC_HOOK_CODE, silent, begin=p, end=p)
boot_component(arm)

src = arm.alloc(len(raw))
arm.uc.mem_write(src, raw)
name_bytes = b"PlFx.dat\0"
name = arm.alloc(len(name_bytes))
arm.uc.mem_write(name, name_bytes)
arm.call("mv_gameplay_archive_prepare_raw", src, len(raw), name)

for off, expected_opcode in commands.items():
    native = struct.unpack("<I", arm.uc.mem_read(src + 32 + off, 4))[0]
    assert native & 0x3F == expected_opcode, (hex(off), expected_opcode, hex(native))
for off, expected in pointer_words.items():
    actual = bytes(arm.uc.mem_read(src + 32 + off, 4))
    assert actual == expected, (hex(off), actual.hex(), expected.hex())
for off in landing_bad:
    native = struct.unpack("<I", arm.uc.mem_read(src + 32 + off, 4))[0]
    assert native == 0x04040037, (hex(off), hex(native))
    assert native & 0x3F == 55 and native & 0x3F != 28

print(
    f"PASS fighter command endian: Fox motions={motion_count} roots={len(set(script_roots))} "
    f"commands={len(commands)} pointer_words={len(pointer_words)} landing55={len(landing_bad)}; "
    "DC000404 no longer aliases ARM SetHurtState opcode 28"
)
