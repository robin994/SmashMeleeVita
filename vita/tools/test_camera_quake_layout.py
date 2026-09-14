#!/usr/bin/env python3
"""Regression: gameplay quake math must not depend on GameCube linker placement."""
from pathlib import Path
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/melee/cm/camera.c"
ELF = ROOT / "build/vita-full/melee_vita"
text = SOURCE.read_text()
start = text.index("void Camera_ApplyQuake(")
end = text.index("void Camera_SetQuakeOffset", start)
body = text[start:end]
assert "CameraStaticData" not in body
assert "(struct CameraStaticData*) &cm_803BCB18" not in body
for token in ("cm_803BCB64.aspect", "cm_803BCB64.viewport.xmax", "cm_803BCB64.viewport.xmin", "cm_803BCB64.viewport.ymax", "cm_803BCB64.viewport.ymin"):
    assert token in body, token

with ELF.open("rb") as stream:
    elf = ELFFile(stream)
    symtab = elf.get_section_by_name(".symtab")
    symbols = {s.name: s["st_value"] for s in symtab.iter_symbols()}
callback = symbols["cm_803BCB18"]
desc = symbols["cm_803BCB64"]
assert callback != desc
gap = abs(desc - callback)
print(f"PASS camera quake layout: direct cm_803BCB64 descriptor access; callback=0x{callback:08x} desc=0x{desc:08x} gap=0x{gap:x}; no linker-placement alias")
