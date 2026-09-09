#!/usr/bin/env python3
"""Extract the user's GALE01 v1.02 disc into ignored orig/GALE01/{sys,files}.

FST layout reference: Dolphin DiscIO/FileSystemGCWii.cpp. Original data is
read-only; existing outputs are accepted only when byte-identical.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tempfile

DOL_SHA1 = '08e0bf20134dfcb260699671004527b2d6bb1a45'

def be32(data, offset):
    return struct.unpack_from('>I', data, offset)[0]

def read_at(stream, offset, size, total):
    if offset < 0 or size < 0 or offset > total or size > total - offset:
        raise ValueError(f'disc range out of bounds: {offset:#x}+{size:#x}')
    stream.seek(offset)
    data = stream.read(size)
    if len(data) != size:
        raise ValueError('short read')
    return data

def parse_fst(fst, disc_size):
    if len(fst) < 12 or be32(fst, 0) >> 24 != 1 or be32(fst, 4) != 0:
        raise ValueError('invalid FST root')
    count = be32(fst, 8)
    if not count or count > len(fst) // 12:
        raise ValueError('invalid FST entry count')
    stack = [(0, count, Path())]
    files, seen = [], set()
    for index in range(1, count):
        while index >= stack[-1][1]:
            stack.pop()
        name_word, offset, size = struct.unpack_from('>III', fst, index * 12)
        name_offset = count * 12 + (name_word & 0xffffff)
        end = fst.find(b'\0', name_offset)
        if name_offset >= len(fst) or end < 0:
            raise ValueError('invalid FST name range')
        name = fst[name_offset:end].decode('ascii')
        if not name or name in ('.', '..') or any(c in name for c in '/\\:'):
            raise ValueError('unsafe FST name')
        path = stack[-1][2] / name
        key = str(path).casefold()
        if key in seen:
            raise ValueError(f'duplicate FST path: {path}')
        seen.add(key)
        kind = name_word >> 24
        if kind == 1:
            if offset != stack[-1][0] or not index < size <= stack[-1][1]:
                raise ValueError('invalid FST directory range/parent')
            stack.append((index, size, path))
        elif kind == 0:
            if offset > disc_size or size > disc_size - offset:
                raise ValueError('invalid FST file range')
            files.append((path, offset, size))
        else:
            raise ValueError('invalid FST entry kind')
    return files

def write_original(root, relative, data):
    if root.is_symlink() or relative.is_absolute() or '..' in relative.parts:
        raise ValueError('unsafe output root/path')
    root = root.resolve()
    destination = root / relative
    # Refuse any existing symlink in the output path (including directories).
    for component in (destination, *destination.parents):
        if component.is_symlink():
            raise ValueError(f'symlink in output path: {component}')
    digest = hashlib.sha256(data).hexdigest()
    if destination.exists():
        if hashlib.sha256(destination.read_bytes()).hexdigest() != digest:
            raise ValueError(f'existing output differs: {destination}')
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as temp:
            temp.write(data)
            temporary = Path(temp.name)
        try:
            # Hard link is atomic and refuses replacement if a file appears meanwhile.
            destination.hardlink_to(temporary)
        finally:
            temporary.unlink()
    return {'path': str(relative), 'bytes': len(data), 'sha256': digest}

def extract(iso, output):
    size = iso.stat().st_size
    with iso.open('rb') as stream:
        header = read_at(stream, 0, 0x440, size)
        if header[:8] != b'GALE01\0\2' or be32(header, 0x1c) != 0xc2339f3d:
            raise ValueError('expected GameCube GALE01 disc revision 2 (USA 1.02)')
        dol_offset, fst_offset, fst_size = struct.unpack_from('>III', header, 0x420)
        if fst_size > 128 * 1024 * 1024:
            raise ValueError('unreasonable FST size')
        dol_header = read_at(stream, dol_offset, 256, size)
        sections = [(be32(dol_header, i * 4), be32(dol_header, 0x90 + i * 4))
                    for i in range(18) if be32(dol_header, 0x90 + i * 4)]
        if not sections or any(offset < 256 for offset, _ in sections):
            raise ValueError('invalid DOL sections')
        dol_size = max(offset + length for offset, length in sections)
        dol = read_at(stream, dol_offset, dol_size, size)
        if hashlib.sha1(dol).hexdigest() != DOL_SHA1:
            raise ValueError('DOL hash does not match upstream USA 1.02')
        fst = read_at(stream, fst_offset, fst_size, size)
        entries = parse_fst(fst, size)
        records = [write_original(output, Path('sys/main.dol'), dol),
                   write_original(output, Path('sys/fst.bin'), fst),
                   write_original(output, Path('sys/boot.bin'), header)]
        for path, offset, length in entries:
            records.append(write_original(output, Path('files') / path,
                                          read_at(stream, offset, length, size)))
    manifest = {'disc_id': 'GALE01', 'revision': 2, 'iso_bytes': size,
                'dol_sha1': DOL_SHA1, 'disc_files': len(entries), 'outputs': records}
    write_original(output, Path('extraction-manifest.json'),
                   (json.dumps(manifest, indent=2) + '\n').encode())
    return manifest

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    parser.add_argument('--output', type=Path, default=Path('orig/GALE01'))
    args = parser.parse_args()
    report = extract(args.iso, args.output)
    print(f"DOL SHA-1 verified: {report['dol_sha1']}")
    print(f"Extracted/verified {report['disc_files']} disc files into {args.output}")
