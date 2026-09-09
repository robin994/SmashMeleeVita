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
args = parser.parse_args()
manifest = json.loads((args.orig/'extraction-manifest.json').read_text())
if manifest['dol_sha1'] != '08e0bf20134dfcb260699671004527b2d6bb1a45':
    raise SystemExit('Unexpected extraction version')
record = next(r for r in manifest['outputs'] if r['path'] == 'files/MnMaAll.usd')
data = (args.orig/record['path']).read_bytes()
if len(data) != record['bytes'] or hashlib.sha256(data).hexdigest() != record['sha256']:
    raise SystemExit('Menu asset differs from verified extraction')
args.output.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(args.output, 'w', compression=zipfile.ZIP_DEFLATED) as package:
    package.writestr('SmashMeleeVita/files/MnMaAll.usd', data)
    package.writestr('SmashMeleeVita/asset-manifest.json', json.dumps(record, indent=2)+'\n')
with zipfile.ZipFile(args.output) as package:
    assert package.testzip() is None
    assert hashlib.sha256(package.read('SmashMeleeVita/files/MnMaAll.usd')).hexdigest() == record['sha256']
print(args.output)
print('Extract under ux0:data; asset SHA-256:', record['sha256'])
