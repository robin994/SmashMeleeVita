#!/usr/bin/env python3
"""Guard the Vita Venom callback-table fix against DOL-layout regressions."""
import argparse
import re
import struct
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--elf", required=True)
args = p.parse_args()
elf_path = Path(args.elf)
root = Path(__file__).resolve().parents[2]
src = (root / "src/melee/gr/grvenom.c").read_text()

m = re.search(r"Ground_GObj\* grVenom_80203EAC\(int gobj_id\)\n\{(.*?)\n\}", src, re.S)
assert m, "grVenom_80203EAC not found"
body = m.group(1)
assert "#ifdef MELEE_VITA_PLATFORM" in body
assert "&grVe_StageCallbacks[gobj_id]" in body
assert "((char*) base + 0x44)" in body  # retained only for non-Vita matching

nm = subprocess.check_output([
    "/usr/local/vitasdk/bin/arm-vita-eabi-nm", "-n", str(elf_path)
], text=True)
symbols = {}
for line in nm.splitlines():
    parts = line.split()
    if len(parts) >= 3:
        try:
            symbols[parts[2]] = int(parts[0], 16)
        except ValueError:
            pass
for name in ("grVe_StageCallbacks", "grVe_803E5348", "grVenom_80203FD4"):
    assert name in symbols, f"missing ELF symbol {name}"

# The whole bug exists because the Vita linker does not preserve the DOL offset.
assert symbols["grVe_StageCallbacks"] != symbols["grVe_803E5348"] + 0x44

data = elf_path.read_bytes()
assert data[:4] == b"\x7fELF" and data[4] == 1 and data[5] == 1
phoff = struct.unpack_from("<I", data, 28)[0]
phentsz = struct.unpack_from("<H", data, 42)[0]
phnum = struct.unpack_from("<H", data, 44)[0]

def read_vaddr(addr, size):
    for i in range(phnum):
        off = phoff + i * phentsz
        p_type, p_offset, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from("<IIIIIIII", data, off)
        if p_type == 1 and p_vaddr <= addr and addr + size <= p_vaddr + p_filesz:
            foff = p_offset + (addr - p_vaddr)
            return data[foff:foff + size]
    raise AssertionError(f"ELF address 0x{addr:x} not file-backed")

# StageCallbacks is five u32 words. Entry 4's on_init must be grVenom_80203FD4.
entry4 = read_vaddr(symbols["grVe_StageCallbacks"] + 4 * 20, 20)
words = struct.unpack("<IIIII", entry4)
assert (words[0] & ~1) == (symbols["grVenom_80203FD4"] & ~1), (
    f"Venom map4 on_init mismatch: 0x{words[0]:08x} != 0x{symbols['grVenom_80203FD4']:08x} (Thumb-normalized)"
)
# All non-null callbacks in the table must point into the executable LOAD range.
rx_lo = rx_hi = None
for i in range(phnum):
    off = phoff + i * phentsz
    p_type, _, p_vaddr, _, _, p_memsz, p_flags, _ = struct.unpack_from("<IIIIIIII", data, off)
    if p_type == 1 and (p_flags & 1):
        rx_lo, rx_hi = p_vaddr, p_vaddr + p_memsz
        break
assert rx_lo is not None
for idx in range(16):
    raw = read_vaddr(symbols["grVe_StageCallbacks"] + idx * 20, 20)
    w = struct.unpack("<IIIII", raw)
    for ptr in w[:4]:
        if ptr:
            assert rx_lo <= (ptr & ~1) < rx_hi, f"entry {idx} callback 0x{ptr:08x} outside RX"
print("PASS Venom callbacks: Vita uses the real table; all 16 entries contain executable callbacks")
