#!/usr/bin/env python3
"""Extract Melee's default SIS glyph atlas from the verified GALE01 1.02 DOL."""
import argparse
import hashlib
import struct
from pathlib import Path

DOL_SHA1 = "08e0bf20134dfcb260699671004527b2d6bb1a45"
ATLAS_ADDR = 0x8040CD40
GLYPH_COUNT = 287
GLYPH_BYTES = 512
ATLAS_BYTES = GLYPH_COUNT * GLYPH_BYTES


def dol_slice(dol: bytes, address: int, size: int) -> bytes:
    if len(dol) < 0x100:
        raise ValueError("DOL header truncated")
    text_offsets = struct.unpack_from(">7I", dol, 0x00)
    data_offsets = struct.unpack_from(">11I", dol, 0x1C)
    text_addrs = struct.unpack_from(">7I", dol, 0x48)
    data_addrs = struct.unpack_from(">11I", dol, 0x64)
    text_sizes = struct.unpack_from(">7I", dol, 0x90)
    data_sizes = struct.unpack_from(">11I", dol, 0xAC)
    sections = list(zip(text_offsets, text_addrs, text_sizes)) + list(
        zip(data_offsets, data_addrs, data_sizes)
    )
    for offset, base, length in sections:
        if not length or address < base or address + size > base + length:
            continue
        file_offset = offset + address - base
        if file_offset + size > len(dol):
            raise ValueError("DOL section exceeds file")
        return dol[file_offset:file_offset + size]
    raise ValueError(f"address 0x{address:08x} is outside DOL sections")


def emit_c(atlas: bytes, output: Path) -> None:
    lines = [
        "/* Generated from the user's verified GALE01 1.02 main.dol. */",
        "#include <sysdolphin/baselib/sislib_font.h>",
        "",
        "TextGlyphTexture HSD_SisLib_FontAtlas[287] __attribute__((aligned(32))) = {",
    ]
    for glyph in range(GLYPH_COUNT):
        chunk = atlas[glyph * GLYPH_BYTES:(glyph + 1) * GLYPH_BYTES]
        lines.append("    { .data = {")
        for off in range(0, GLYPH_BYTES, 16):
            row = ", ".join(f"0x{b:02x}" for b in chunk[off:off + 16])
            lines.append(f"        {row},")
        lines.append("    } },")
    lines.append("};")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dol", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    dol = args.dol.read_bytes()
    digest = hashlib.sha1(dol).hexdigest()
    if digest != DOL_SHA1:
        raise SystemExit(f"unexpected GALE01 DOL SHA-1: {digest}")
    atlas = dol_slice(dol, ATLAS_ADDR, ATLAS_BYTES)
    if len(atlas) != ATLAS_BYTES or not any(atlas):
        raise SystemExit("SIS font atlas extraction failed")
    emit_c(atlas, args.output)
    print(f"SIS font atlas: {len(atlas)} bytes -> {args.output}")


if __name__ == "__main__":
    main()
