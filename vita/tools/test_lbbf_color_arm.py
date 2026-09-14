#!/usr/bin/env python3
"""ARM regression for LbBf.dat ColorOverlay command nativization/runtime."""
from pathlib import Path
import math
import struct

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R2

ROOT = Path(__file__).resolve().parents[2]
ASSET = ROOT / "orig/GALE01/files/LbBf.dat"
ELF = ROOT / "build/vita-full/melee_vita"


def parse_dat(raw: bytes):
    data_size, reloc_count, public_count, extern_count = struct.unpack_from(">4I", raw, 4)
    reloc_base = 32 + data_size
    public_base = reloc_base + reloc_count * 4
    strings_base = public_base + (public_count + extern_count) * 8
    relocs = {struct.unpack_from(">I", raw, reloc_base + i * 4)[0] for i in range(reloc_count)}
    publics = {}
    for i in range(public_count):
        target, name_off = struct.unpack_from(">2I", raw, public_base + i * 8)
        start = strings_base + name_off
        end = raw.index(0, start)
        publics[raw[start:end].decode("ascii")] = target
    return data_size, relocs, publics


def cstring(arm, text):
    raw = text.encode("ascii") + b"\0"
    p = arm.alloc(len(raw))
    arm.uc.mem_write(p, raw)
    return p


raw = ASSET.read_bytes()
data_size, relocs, publics = parse_dat(raw)
root = publics["lbBgFlashColAnimData"]
assert data_size - root == 16 * 8, (hex(root), data_size)

# Damage flash IDs used by ftCo_Damage. Retail scripts are:
# set-color(op18), interpolate(op19, 12f), wait(op11, 12f), disable(op12), end(op10).
expected_rgb = {
    2: ((0xFF, 0xFF, 0xFF), (0xFF, 0xFF, 0xFF)),
    3: ((0xFF, 0x8C, 0x78), (0xFF, 0x8C, 0x78)),
    4: ((0x8C, 0x8C, 0xFF), (0xFF, 0x8C, 0xFF)),
    5: ((0x00, 0x80, 0xFF), (0xFF, 0x80, 0xFF)),
    6: ((0xFF, 0x00, 0x78), (0xFF, 0x00, 0x78)),
}
for ident, (start_rgb, target_rgb) in expected_rgb.items():
    field = root + ident * 8
    assert field in relocs
    script = struct.unpack_from(">I", raw, 32 + field)[0]
    words = [struct.unpack_from(">I", raw, 32 + script + i * 4)[0] for i in range(7)]
    assert words[0] >> 26 == 18
    assert words[2] >> 26 == 19 and (words[2] & 0x03FFFFFF) == 12
    assert words[4] >> 26 == 11 and (words[4] & 0x03FFFFFF) == 12
    assert words[5] >> 26 == 12
    assert words[6] >> 26 == 10
    assert tuple(raw[32 + script + 4:32 + script + 7]) == start_rgb
    assert tuple(raw[32 + script + 12:32 + script + 15]) == target_rgb
    assert raw[32 + script + 7] == 12

arm = ArmHarness(str(ELF))


def silent(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


def panic(machine, _address, _size, _data):
    msg = arm.string(machine.reg_read(UC_ARM_REG_R2)).decode(errors="replace")
    raise AssertionError("ARM HSD_Panic: " + msg)


for name, hook in (("OSReport", silent), ("HSD_Panic", panic)):
    if name in arm.symbols:
        addr = arm.symbols[name] & ~1
        arm.uc.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)
boot_component(arm)

src = arm.alloc(len(raw))
arm.uc.mem_write(src, raw)
arm.call("mv_gameplay_archive_prepare_raw", src, len(raw), cstring(arm, "LbBf.dat"))
data = src + 32

# Pointer words remain BE for HSD_ArchiveParse; packed script words become
# native little-endian values matching ARM's bitfield layout.
for ident in expected_rgb:
    field = root + ident * 8
    assert bytes(arm.uc.mem_read(data + field, 4)) == raw[32 + field:32 + field + 4]
    script = struct.unpack_from(">I", raw, 32 + field)[0]
    got = [struct.unpack("<I", arm.uc.mem_read(data + script + i * 4, 4))[0] for i in (0, 2, 4, 5, 6)]
    want = [18, 19 | (12 << 6), 11 | (12 << 6), 12, 10]
    assert got == want, (ident, [hex(x) for x in got], [hex(x) for x in want])
    # RGBA payload words are data, not command words, and stay byte-for-byte.
    assert bytes(arm.uc.mem_read(data + script + 4, 4)) == raw[32 + script + 4:32 + script + 8]
    assert bytes(arm.uc.mem_read(data + script + 12, 4)) == raw[32 + script + 12:32 + script + 16]

archive = arm.alloc(0x44)
assert arm.call("HSD_ArchiveParse", archive, src, len(raw)) == 0
table = arm.call("HSD_ArchiveGetPublicAddress", archive, cstring(arm, "lbBgFlashColAnimData"))
assert table

# Execute the actual damage-flash scripts through linked ARM lb_013B.c.
for ident, (start_rgb, target_rgb) in expected_rgb.items():
    co = arm.alloc(0x80)
    arm.uc.mem_write(co, b"\0" * 0x80)
    arm.call("lb_80014498", co)
    assert arm.call("lb_800144C8", co, table, ident, 0) == 1
    alphas = []
    enabled_seen = False
    terminated = False
    for frame in range(32):
        done = arm.call("lb_80014258", 0, co, 0)
        flags = arm.uc.mem_read(co + 0x7C, 1)[0]
        enabled = bool(flags & 0x01)
        rgba = tuple(arm.uc.mem_read(co + 0x2C, 4))
        floats = struct.unpack("<8f", arm.uc.mem_read(co + 0x30, 32))
        assert all(math.isfinite(v) for v in floats), (ident, frame, floats)
        if enabled:
            enabled_seen = True
            for value, start, target in zip(rgba[:3], start_rgb, target_rgb):
                assert min(start, target) <= value <= max(start, target), (ident, frame, rgba, start_rgb, target_rgb)
            alphas.append(rgba[3])
        if done:
            terminated = True
            break
    assert enabled_seen, ident
    assert terminated, ident
    assert alphas and alphas[0] <= 12 and alphas[-1] <= alphas[0], (ident, alphas)
    assert all(a >= b for a, b in zip(alphas, alphas[1:])), (ident, alphas)
    assert len(alphas) <= 13, (ident, alphas)

print("PASS LbBf ColorOverlay ARM: damage flash IDs 2-6 nativeize to op18/op19/op11/op12/op10, fade finite alpha 12->0, terminate, and preserve relocation/color payload bytes")
