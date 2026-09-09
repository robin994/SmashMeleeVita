#!/usr/bin/env python3
"""Create a local Vita data ZIP from the verified extraction; never add it to VPK/Git."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--orig', type=Path, default=ROOT/'orig/GALE01')
parser.add_argument('--output', type=Path, default=ROOT/'build/vita/SmashMeleeVita-menu-assets.zip')
parser.add_argument('--boot', action='store_true', help='Include original memory-card scene archives')
args = parser.parse_args()
manifest = json.loads((args.orig/'extraction-manifest.json').read_text())
if manifest['dol_sha1'] != '08e0bf20134dfcb260699671004527b2d6bb1a45':
    raise SystemExit('Unexpected extraction version')
names = ['MnMaAll.usd']
if args.boot:
    names += ['LbMcGame.dat', 'LbMcGame.usd', 'NtMemAc.dat', 'NtMemAc.usd',
              'NtMsgWin.dat', 'SdMsgBox.dat', 'SdMsgBox.usd']
records = {r['path']: r for r in manifest['outputs']}
verified = []
for name in names:
    record = records['files/' + name]
    data = (args.orig/record['path']).read_bytes()
    if len(data) != record['bytes'] or hashlib.sha256(data).hexdigest() != record['sha256']:
        raise SystemExit('Asset differs from verified extraction: ' + name)
    verified.append((name, record, data))
args.output.parent.mkdir(parents=True, exist_ok=True)
import os
import tempfile
fd, temporary = tempfile.mkstemp(prefix=args.output.name + '.', dir=args.output.parent)
os.close(fd)
try:
    with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED) as package:
        for name, record, data in verified:
            package.writestr('SmashMeleeVita/files/' + name, data)
        package.writestr('SmashMeleeVita/asset-manifest.json',
                        json.dumps([r for _, r, _ in verified], indent=2)+'\n')
    with zipfile.ZipFile(temporary) as package:
        assert package.testzip() is None
        for name, record, _ in verified:
            assert hashlib.sha256(package.read('SmashMeleeVita/files/' + name)).hexdigest() == record['sha256']
    os.replace(temporary, args.output)
finally:
    if os.path.exists(temporary): os.unlink(temporary)
print(args.output)
print('Extract under ux0:data; verified original files:', len(verified))
