#!/usr/bin/env python3
"""Inspect material/texture/shape animation trees in a big-endian HSD archive."""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import struct


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--material", default="MenMainBack_Top_matanim_joint")
    parser.add_argument("--shape", default="MenMainBack_Top_shapeanim_joint")
    args = parser.parse_args()
    blob = args.archive.read_bytes()
    size, data_size, reloc_count, public_count, extern_count = struct.unpack_from(">IIIII", blob, 0)
    if size != len(blob):
        raise SystemExit("archive size mismatch")
    data = 32
    reloc = data + data_size
    public = reloc + reloc_count * 4
    external = public + public_count * 8
    strings = external + extern_count * 8

    def be32(off: int) -> int:
        return struct.unpack_from(">I", blob, data + off)[0]

    def raw32(off: int) -> int:
        return struct.unpack_from(">I", blob, off)[0]

    relocations = {raw32(reloc + i * 4) for i in range(reloc_count)}

    def ptr(field: int) -> int | None:
        value = be32(field)
        if field not in relocations:
            if value == 0:
                return None
            raise ValueError(f"non-relocated non-null pointer {field:#x}->{value:#x}")
        if value > data_size:
            raise ValueError(f"pointer outside data {field:#x}->{value:#x}")
        return value

    def cstring(off: int) -> str:
        start = strings + off
        end = blob.index(0, start)
        return blob[start:end].decode("ascii", "replace")

    symbols: dict[str, int] = {}
    for i in range(public_count):
        target, name = struct.unpack_from(">II", blob, public + i * 8)
        symbols[cstring(name)] = target

    def inspect_aobj(aobj: int | None, types: Counter[int]) -> tuple[int, int]:
        if aobj is None:
            return 0, 0
        fobj = ptr(aobj + 8)
        fcount = 0
        seen: set[int] = set()
        while fobj is not None:
            if fobj in seen:
                raise ValueError("FObj cycle")
            seen.add(fobj)
            fcount += 1
            types[blob[data + fobj + 12]] += 1
            fobj = ptr(fobj)
        return 1, fcount

    def inspect_material(root: int) -> None:
        todo = [root]
        seen_joint: set[int] = set()
        joint_count = matanim_count = texanim_count = 0
        mobj_aobj = tex_aobj = chan_aobj = reg_aobj = 0
        fobj_count = 0
        mobj_types: Counter[int] = Counter()
        tex_types: Counter[int] = Counter()
        chan_types: Counter[int] = Counter()
        reg_types: Counter[int] = Counter()
        tex_ids: Counter[int] = Counter()
        image_table_sizes: Counter[int] = Counter()
        tlut_table_sizes: Counter[int] = Counter()
        renderanim_count = chan_count = reg_count = 0
        while todo:
            joint = todo.pop()
            if joint in seen_joint:
                raise ValueError("MatAnimJoint cycle")
            seen_joint.add(joint)
            joint_count += 1
            child, nxt, matanim = ptr(joint), ptr(joint + 4), ptr(joint + 8)
            if nxt is not None:
                todo.append(nxt)
            if child is not None:
                todo.append(child)
            seen_mat: set[int] = set()
            while matanim is not None:
                if matanim in seen_mat:
                    raise ValueError("MatAnim cycle")
                seen_mat.add(matanim)
                matanim_count += 1
                nxt_mat = ptr(matanim)
                aobj = ptr(matanim + 4)
                texanim = ptr(matanim + 8)
                renderanim = ptr(matanim + 12)
                a, f = inspect_aobj(aobj, mobj_types)
                mobj_aobj += a; fobj_count += f
                seen_tex: set[int] = set()
                while texanim is not None:
                    if texanim in seen_tex:
                        raise ValueError("TexAnim cycle")
                    seen_tex.add(texanim)
                    texanim_count += 1
                    tex_ids[be32(texanim + 4)] += 1
                    a, f = inspect_aobj(ptr(texanim + 8), tex_types)
                    tex_aobj += a; fobj_count += f
                    n_image, n_tlut = struct.unpack_from(">HH", blob, data + texanim + 20)
                    image_table_sizes[n_image] += 1
                    tlut_table_sizes[n_tlut] += 1
                    texanim = ptr(texanim)
                if renderanim is not None:
                    renderanim_count += 1
                    chan = ptr(renderanim)
                    reg = ptr(renderanim + 4)
                    seen_chan: set[int] = set()
                    while chan is not None:
                        if chan in seen_chan: raise ValueError("ChanAnim cycle")
                        seen_chan.add(chan); chan_count += 1
                        a, f = inspect_aobj(ptr(chan + 4), chan_types)
                        chan_aobj += a; fobj_count += f
                        chan = ptr(chan)
                    seen_reg: set[int] = set()
                    while reg is not None:
                        if reg in seen_reg: raise ValueError("TevRegAnim cycle")
                        seen_reg.add(reg); reg_count += 1
                        a, f = inspect_aobj(ptr(reg + 4), reg_types)
                        reg_aobj += a; fobj_count += f
                        reg = ptr(reg)
                matanim = nxt_mat
        print(f"material root={args.material} joint_nodes={joint_count} matanim={matanim_count} texanim={texanim_count} renderanim={renderanim_count}")
        print(f"material aobj: mobj={mobj_aobj} tex={tex_aobj} chan={chan_aobj} reg={reg_aobj} fobj_total={fobj_count}")
        print(f"material fobj_types={dict(sorted(mobj_types.items()))}")
        print(f"texture fobj_types={dict(sorted(tex_types.items()))} tex_ids={dict(sorted(tex_ids.items()))}")
        print(f"texture image_table_sizes={dict(sorted(image_table_sizes.items()))} tlut_table_sizes={dict(sorted(tlut_table_sizes.items()))}")
        print(f"render chan={chan_count} reg={reg_count} chan_types={dict(sorted(chan_types.items()))} reg_types={dict(sorted(reg_types.items()))}")

    def inspect_shape(root: int) -> None:
        todo = [root]
        seen_joint: set[int] = set()
        joint_count = dobj_count = shape_count = aobj_count = fobj_count = 0
        types: Counter[int] = Counter()
        while todo:
            joint = todo.pop()
            if joint in seen_joint: raise ValueError("ShapeAnimJoint cycle")
            seen_joint.add(joint); joint_count += 1
            child, nxt, dobj = ptr(joint), ptr(joint + 4), ptr(joint + 8)
            if nxt is not None: todo.append(nxt)
            if child is not None: todo.append(child)
            seen_dobj: set[int] = set()
            while dobj is not None:
                if dobj in seen_dobj: raise ValueError("ShapeAnimDObj cycle")
                seen_dobj.add(dobj); dobj_count += 1
                shape = ptr(dobj + 4)
                seen_shape: set[int] = set()
                while shape is not None:
                    if shape in seen_shape: raise ValueError("ShapeAnim cycle")
                    seen_shape.add(shape); shape_count += 1
                    a, f = inspect_aobj(ptr(shape + 4), types)
                    aobj_count += a; fobj_count += f
                    shape = ptr(shape)
                dobj = ptr(dobj)
        print(f"shape root={args.shape} joint_nodes={joint_count} dobj_nodes={dobj_count} shapeanim={shape_count} aobj={aobj_count} fobj={fobj_count} types={dict(sorted(types.items()))}")

    if args.material in symbols:
        inspect_material(symbols[args.material])
    else:
        print(f"material symbol missing: {args.material}")
    if args.shape in symbols:
        inspect_shape(symbols[args.shape])
    else:
        print(f"shape symbol missing: {args.shape}")


if __name__ == "__main__":
    main()
