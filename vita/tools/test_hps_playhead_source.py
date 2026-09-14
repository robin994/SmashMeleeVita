#!/usr/bin/env python3
from pathlib import Path

src = (Path(__file__).resolve().parents[2] / "src/sysdolphin/baselib/synth.c").read_text()
if "HSD_SynthVoiceCurrentAddr(node->voice[0])" not in src:
    raise SystemExit("FAIL HPS playhead: stream path does not use semantic AX address helper")
if "currentAddressHi << 16" not in src or "currentAddressLo" not in src:
    raise SystemExit("FAIL HPS playhead: helper does not reconstruct high/low halfwords")
if "*(u32*) ((u8*) node->voice[0] + 0x1B2)" in src:
    raise SystemExit("FAIL HPS playhead: PPC endian-dependent u32 alias still present")
hi, lo = 0x0123, 0x4567
semantic = (hi << 16) | lo
little_alias = (lo << 16) | hi
if semantic != 0x01234567 or little_alias == semantic:
    raise SystemExit("FAIL HPS playhead regression arithmetic")
print("PASS HPS playhead endian: AX currentAddressHi/Lo are reconstructed semantically on ARM")
