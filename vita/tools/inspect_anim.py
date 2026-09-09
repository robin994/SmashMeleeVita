#!/usr/bin/env python3
"""Inspect one HSD AnimJoint tree from a verified big-endian DAT/USD archive."""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import struct


HSD_A_OP_CON = 1
HSD_A_OP_LIN = 2
HSD_A_OP_SPL0 = 3
HSD_A_OP_SPL = 4
HSD_A_OP_SLP = 5
HSD_A_OP_KEY = 6


def hermite(fterm: float, time: float, p0: float, p1: float, d0: float, d1: float) -> float:
    time2 = time * time
    term2 = fterm * fterm
    time2_term = time2 * fterm
    term2_time3 = term2 * (time2 * time)
    two_time3_term3 = 2.0 * term2_time3 * fterm
    three_time2_term2 = 3.0 * time2 * term2
    return d1 * (term2_time3 - time2_term) + (
        d0 * (time + ((term2_time3 - time2_term) - time2_term))
        + p0 * (1.0 + (two_time3_term3 - three_time2_term2))
        + p1 * (-two_time3_term3 + three_time2_term2)
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("symbol", nargs="?", default="MenMainBack_Top_animjoint")
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

    def be32(offset: int) -> int:
        return struct.unpack_from(">I", blob, offset)[0]

    relocations = {be32(reloc + i * 4) for i in range(reloc_count)}

    def pointer(field: int) -> int | None:
        value = be32(data + field)
        if field not in relocations:
            if value == 0:
                return None
            raise ValueError(f"non-relocated non-null pointer field {field:#x} -> {value:#x}")
        if value > data_size:
            raise ValueError(f"pointer outside data {field:#x} -> {value:#x}")
        return value

    def cstring(offset: int) -> str:
        start = strings + offset
        end = blob.index(0, start)
        return blob[start:end].decode("ascii", "replace")

    def eval_fobj(desc: int, frame: float) -> tuple[int, float] | None:
        """Match the first HSD_FObjInterpretAnim call after ReqAnim(frame)."""
        length = be32(data + desc + 4)
        start_frame = struct.unpack_from(">f", blob, data + desc + 8)[0]
        obj_type = blob[data + desc + 12]
        frac_value = blob[data + desc + 13]
        frac_slope = blob[data + desc + 14]
        ad_offset = pointer(desc + 16)
        if ad_offset is None:
            raise ValueError(f"FObjDesc {desc:#x} has no AD stream")
        ad = memoryview(blob)[data + ad_offset:data + ad_offset + length]
        pos = 0
        state = 1
        time = start_frame + frame
        op = 0
        op_intrp = 0
        flags = 0
        nb_pack = 0
        fterm = 0
        p0 = p1 = d0 = d1 = 0.0

        def need(count: int) -> None:
            if pos + count > len(ad):
                raise ValueError(f"FObjDesc {desc:#x} truncated AD stream")

        def parse_float(frac: int) -> float:
            nonlocal pos
            fmt = frac & 0xE0
            denom = float(1 << (frac & 0x1F))
            if fmt == 0:
                need(4)
                value = struct.unpack_from("<f", ad, pos)[0]
                pos += 4
                return value
            if fmt == 0x60:
                need(1); value = struct.unpack_from("b", ad, pos)[0]; pos += 1
            elif fmt == 0x80:
                need(1); value = ad[pos]; pos += 1
            elif fmt == 0x20:
                need(2); value = struct.unpack_from("<h", ad, pos)[0]; pos += 2
            elif fmt == 0x40:
                need(2); value = struct.unpack_from("<H", ad, pos)[0]; pos += 2
            else:
                raise ValueError(f"unsupported FObj fraction {frac:#x}")
            return float(value) / denom

        def parse_pack_info() -> int:
            nonlocal pos
            need(1)
            first = ad[pos]; pos += 1
            packs = ((first >> 4) & 7) + 1
            shift = 3
            byte = first
            while byte & 0x80:
                need(1)
                byte = ad[pos]; pos += 1
                packs += (byte & 0x7F) << shift
                shift += 7
            return packs

        def parse_wait() -> int:
            nonlocal pos
            wait = 0
            shift = 0
            while True:
                need(1)
                byte = ad[pos]; pos += 1
                wait |= (byte & 0x7F) << shift
                shift += 7
                if not byte & 0x80:
                    return wait

        def update() -> float | None:
            nonlocal flags, d0, p0
            if op_intrp == HSD_A_OP_KEY:
                if flags & 0x80:
                    flags &= ~0x80
                    return p0
                return None
            if op_intrp == HSD_A_OP_CON:
                return p1 if time >= fterm else p0
            if op_intrp == HSD_A_OP_LIN:
                if flags & 0x20:
                    flags &= ~0x20
                    if fterm:
                        d0 = (p1 - p0) / fterm
                    else:
                        d0 = 0.0
                        p0 = p1
                return d0 * time + p0
            if op_intrp in (HSD_A_OP_SPL0, HSD_A_OP_SPL, HSD_A_OP_SLP):
                return hermite(1.0 / fterm, time, p0, p1, d0, d1) if fterm else p1
            return None

        if time < 0.0:
            return None
        carried = 0.0
        for _ in range(100000):
            if state == 6:
                time += carried
                if flags & 0x40:
                    op_intrp = op
                    flags &= ~0x40
                    flags |= 0x80
                    p0 = p1
                value = update()
                return (obj_type, value) if value is not None else None
            if state in (1, 2):
                if pos >= len(ad):
                    state = 6
                    continue
                op_intrp = op
                if nb_pack == 0:
                    op = ad[pos] & 0xF
                    nb_pack = parse_pack_info()
                nb_pack -= 1
                old_state = state
                if op == HSD_A_OP_CON:
                    p0 = p1; p1 = parse_float(frac_value)
                    if op_intrp != HSD_A_OP_SLP: d0 = d1; d1 = 0.0
                    state = 3 if old_state == 1 else 4
                elif op == HSD_A_OP_LIN:
                    p0 = p1; p1 = parse_float(frac_value)
                    if op_intrp != HSD_A_OP_SLP: d0 = d1; d1 = 0.0
                    state = 3 if old_state == 1 else 4
                elif op == HSD_A_OP_SPL0:
                    p0 = p1; d0 = d1; p1 = parse_float(frac_value); d1 = 0.0
                    state = 3 if old_state == 1 else 4
                elif op == HSD_A_OP_SPL:
                    p0 = p1; p1 = parse_float(frac_value); d0 = d1; d1 = parse_float(frac_slope)
                    state = 3 if old_state == 1 else 4
                elif op == HSD_A_OP_SLP:
                    d0 = d1; d1 = parse_float(frac_slope)
                elif op == HSD_A_OP_KEY:
                    if flags & 0x40:
                        op_intrp = op; flags &= ~0x40; flags |= 0x80; p0 = p1
                    p1 = parse_float(frac_value); flags |= 0x40
                    state = 3 if old_state == 1 else 4
                else:
                    return None
                continue
            if state == 3:
                if flags & 0x80:
                    value = update()
                    if value is not None:
                        # Upstream continues after the key callback. The last callback
                        # in this first interpretation is what the JObj retains.
                        pass
                if pos >= len(ad):
                    state = 6
                else:
                    fterm = parse_wait()
                    flags |= 0x20
                    state = 2
                continue
            if state == 4:
                if fterm <= time:
                    state = 3
                    carried = float(fterm)
                    time -= fterm
                    continue
                value = update()
                return (obj_type, value) if value is not None else None
            if state == 5:
                state = 4
                continue
            if state == 0:
                return None
        raise ValueError(f"FObjDesc {desc:#x} interpreter guard exceeded")

    symbols: dict[str, int] = {}
    for i in range(public_count):
        target, name = struct.unpack_from(">II", blob, public + i * 8)
        symbols[cstring(name)] = target
    if args.symbol not in symbols:
        raise SystemExit(f"symbol not found: {args.symbol}")

    root = symbols[args.symbol]
    todo = [(root, 0)]
    seen: set[int] = set()
    nodes: list[tuple[int, int, int | None, int | None, int | None, int | None, int]] = []
    anim_flags: Counter[int] = Counter()
    fobj_types: Counter[int] = Counter()
    aobj_count = 0
    aobj_obj_ids = Counter()
    aobj_flags = Counter()
    robj_anim_count = 0
    fobj_count = 0
    max_end = 0.0
    starts: list[float] = []
    lengths: list[int] = []

    while todo:
        offset, depth = todo.pop()
        if offset in seen:
            raise ValueError(f"AnimJoint cycle at {offset:#x}")
        seen.add(offset)
        child = pointer(offset)
        next_joint = pointer(offset + 4)
        aobj = pointer(offset + 8)
        robj = pointer(offset + 12)
        flags = be32(data + offset + 16)
        nodes.append((offset, depth, child, next_joint, aobj, robj, flags))
        anim_flags[flags] += 1
        if aobj is not None:
            aobj_count += 1
            aobj_flags[be32(data + aobj)] += 1
            aobj_obj_ids[be32(data + aobj + 12)] += 1
            end_frame = struct.unpack_from(">f", blob, data + aobj + 4)[0]
            max_end = max(max_end, end_frame)
            fobj = pointer(aobj + 8)
            guard = 0
            while fobj is not None:
                guard += 1
                if guard > 1024:
                    raise ValueError("FObjDesc list too long")
                fobj_count += 1
                next_fobj = pointer(fobj)
                length = be32(data + fobj + 4)
                start_frame = struct.unpack_from(">f", blob, data + fobj + 8)[0]
                fobj_type = blob[data + fobj + 12]
                fobj_types[fobj_type] += 1
                starts.append(start_frame)
                lengths.append(length)
                fobj = next_fobj
        if robj is not None:
            robj_anim_count += 1
        if next_joint is not None:
            todo.append((next_joint, depth))
        if child is not None:
            todo.append((child, depth + 1))

    print(f"symbol={args.symbol} root={root:#x}")
    print(f"anim_nodes={len(nodes)} aobj={aobj_count} fobjdesc={fobj_count} max_end={max_end:g}")
    print("anim_flags=" + ",".join(f"{key:#x}:{count}" for key, count in sorted(anim_flags.items())))
    print("aobj_flags=" + ",".join(f"{key:#x}:{count}" for key, count in sorted(aobj_flags.items())))
    print("aobj_obj_ids=" + ",".join(f"{key:#x}:{count}" for key, count in sorted(aobj_obj_ids.items())))
    print(f"robj_anim={robj_anim_count}")
    print("fobj_types=" + ",".join(f"{key}:{count}" for key, count in sorted(fobj_types.items())))
    if starts:
        print(f"startframe={min(starts):g}..{max(starts):g} ad_bytes={sum(lengths)} max_ad={max(lengths)}")
    shown = 0
    for offset, depth, _child, _next, aobj, _robj, flags in nodes:
        if aobj is None:
            continue
        end_frame = struct.unpack_from(">f", blob, data + aobj + 4)[0]
        fobj = pointer(aobj + 8)
        types: list[int] = []
        values: list[tuple[int, float]] = []
        while fobj is not None and len(types) < 32:
            types.append(blob[data + fobj + 12])
            evaluated = eval_fobj(fobj, 0.0)
            if evaluated is not None:
                values.append(evaluated)
            fobj = pointer(fobj)
        print(
            f"node={offset:#x} depth={depth} anim_flags={flags:#x} end={end_frame:g} "
            f"types={types} frame0={[ (t, round(v, 6)) for t, v in values ]}"
        )
        shown += 1
        if shown >= 48:
            break


if __name__ == "__main__":
    main()
