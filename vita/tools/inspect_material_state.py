#!/usr/bin/env python3
"""Inspect serialized MObj/TObj/PE state for a named HSD Joint tree."""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import struct


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("archive", type=Path)
    ap.add_argument("root", nargs="?", default="MenMainBack_Top_joint")
    args = ap.parse_args()
    blob = args.archive.read_bytes()
    size, data_size, reloc_count, public_count, extern_count = struct.unpack_from(">IIIII", blob, 0)
    if size != len(blob): raise SystemExit("archive size mismatch")
    data = 32; reloc = data + data_size; public = reloc + reloc_count * 4
    external = public + public_count * 8; strings = external + extern_count * 8
    def raw32(off): return struct.unpack_from(">I", blob, off)[0]
    def be32(off): return struct.unpack_from(">I", blob, data + off)[0]
    def bef(off): return struct.unpack_from(">f", blob, data + off)[0]
    relocs = {raw32(reloc + i*4) for i in range(reloc_count)}
    def ptr(field):
        val = be32(field)
        if field not in relocs:
            if val == 0: return None
            raise ValueError(f"non-relocated pointer {field:#x}->{val:#x}")
        if val > data_size: raise ValueError("pointer outside data")
        return val
    def cstring(off):
        s = strings + off; e = blob.index(0, s); return blob[s:e].decode("ascii", "replace")
    symbols = {}
    for i in range(public_count):
        target, name = struct.unpack_from(">II", blob, public + i*8)
        symbols[cstring(name)] = target
    root = symbols[args.root]
    todo=[root]; joints=set(); dobjs=set(); mobjs=set(); tobjs=set(); pobjs=set()
    rendermodes=Counter(); pe_states=Counter(); tex_flags=Counter(); tex_ids=Counter(); tex_wrap=Counter(); tex_blending=Counter()
    tex_rotate=Counter(); tex_scale=Counter(); tex_translate=Counter()
    tobj_counts=Counter(); mobj_textures=[]; pobj_flags=Counter(); cull_modes=Counter()
    while todo:
        j=todo.pop()
        if j in joints: continue
        joints.add(j)
        child,nxt,dobj=ptr(j+8),ptr(j+12),ptr(j+16)
        if child is not None: todo.append(child)
        if nxt is not None: todo.append(nxt)
        while dobj is not None:
            if dobj in dobjs: break
            dobjs.add(dobj)
            mobj=ptr(dobj+8)
            pobj=ptr(dobj+12)
            while pobj is not None:
                if pobj in pobjs: break
                pobjs.add(pobj)
                flags=struct.unpack_from(">H", blob, data+pobj+12)[0]
                pobj_flags[flags] += 1
                cull_modes[(flags >> 14) & 3] += 1
                pobj=ptr(pobj+4)
            if mobj is not None and mobj not in mobjs:
                mobjs.add(mobj)
                mode=be32(mobj+4); rendermodes[mode]+=1
                pe=ptr(mobj+20)
                if pe is not None:
                    p=blob[data+pe:data+pe+12]
                    pe_states[tuple(p)] += 1
                t=ptr(mobj+8); count=0
                while t is not None:
                    if t in tobjs: break
                    tobjs.add(t); count+=1
                    tex_ids[be32(t+8)] += 1
                    flags=be32(t+64); tex_flags[flags]+=1
                    tex_rotate[tuple(round(bef(t+16+i*4),6) for i in range(3))] += 1
                    tex_scale[tuple(round(bef(t+28+i*4),6) for i in range(3))] += 1
                    tex_translate[tuple(round(bef(t+40+i*4),6) for i in range(3))] += 1
                    tex_wrap[(be32(t+52),be32(t+56),blob[data+t+60],blob[data+t+61])] += 1
                    tex_blending[round(bef(t+68),6)] += 1
                    t=ptr(t+4)
                tobj_counts[count]+=1; mobj_textures.append((mobj,count,mode))
            dobj=ptr(dobj+4)
    print(f"root={args.root} joints={len(joints)} dobjs={len(dobjs)} mobjs={len(mobjs)} tobjs={len(tobjs)}")
    print("rendermodes:")
    for k,v in rendermodes.most_common(): print(f"  {k:#010x}: {v}")
    print("PE states (flags ref0 ref1 dst type src dst logic z acomp0 aop acomp1):")
    for k,v in pe_states.most_common(): print(f"  {k}: {v}")
    print("TObj flags:")
    for k,v in tex_flags.most_common(): print(f"  {k:#010x}: {v}")
    print(f"TObj ids={dict(sorted(tex_ids.items()))}")
    print(f"TObj wrap/repeat={dict(tex_wrap)}")
    print(f"TObj blending={dict(sorted(tex_blending.items()))}")
    print(f"TObj rotate={dict(tex_rotate)}")
    print(f"TObj scale={dict(tex_scale)}")
    print(f"TObj translate={dict(tex_translate)}")
    print(f"TObjs per MObj={dict(sorted(tobj_counts.items()))}")
    print(f"PObj flags={{{', '.join(f'{k:#06x}: {v}' for k,v in sorted(pobj_flags.items()))}}}")
    print(f"PObj cull modes (NONE/FRONT/BACK/ALL = 0/1/2/3)={dict(sorted(cull_modes.items()))}")

if __name__ == "__main__": main()
