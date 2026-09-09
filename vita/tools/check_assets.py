#!/usr/bin/env python3
"""Build host checks with ASan/UBSan, validate local archives and decode menu textures."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--files', type=Path, default=ROOT/'orig/GALE01/files')
    args = parser.parse_args()
    out = ROOT/'build/vita/host'
    out.mkdir(parents=True, exist_ok=True)
    textures = out/'menu-textures'
    textures.mkdir(exist_ok=True)
    flags = [os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-g', '-Ivita', '-Isrc', '-Iextern/dolphin/include']
    core = ['vita/hsd_data.c', 'vita/hsd_anim.c', 'vita/hsd_scene.c', 'vita/hsd_matanim.c',
            'vita/hsd_native.c', 'vita/gx_texture.c']
    subprocess.run([*flags, *core, 'vita/tests/assets_test.c', '-o', str(out/'assets_test')], cwd=ROOT, check=True)
    subprocess.run([str(out/'assets_test')], check=True)
    subprocess.run([*flags, *core, 'vita/asset_file.c', 'vita/tools/asset_probe.c', '-o', str(out/'asset_probe')], cwd=ROOT, check=True)
    with (textures/'report.json').open('w') as report:
        subprocess.run([str(out/'asset_probe'), str(args.files/'MnMaAll.usd'), str(textures)], stdout=report, check=True)
    menu_report = json.loads((textures/'report.json').read_text())
    animated = menu_report.get('animated_scene')
    expected_anim = {
        'meshes': 86, 'triangles': 324, 'joints': 102, 'anim_joints': 102,
        'aobjs': 40, 'fobjs': 87, 'channels': 87, 'unsupported': 0,
    }
    if animated is None or any(animated.get(key) != value for key, value in expected_anim.items()):
        raise SystemExit(f'Unexpected MenMainBack frame-0 animation summary: {animated!r}')
    sample = menu_report.get('first_anim_sample')
    expected_sample = [0.451416016, 0.5809021, 0.217559814, 4.0, -10.0, -53.0]
    actual_sample = (sample or {}).get('rot', []) + (sample or {}).get('translate', [])
    if (sample or {}).get('mask') != 119 or len(actual_sample) != len(expected_sample) or any(
        not math.isclose(actual, expected, rel_tol=1e-6, abs_tol=1e-6)
        for actual, expected in zip(actual_sample, expected_sample)
    ):
        raise SystemExit(f'Unexpected MenMainBack first frame-0 FObj sample: {sample!r}')
    paths = sorted([*args.files.glob('*.dat'), *args.files.glob('*.usd')])
    if not paths:
        raise SystemExit('No extracted archives: run extract_disc.py first')
    def check(path):
        with path.open('rb') as stream: header = stream.read(32)
        if len(header) < 32 or struct.unpack_from('>I', header)[0] != path.stat().st_size:
            return {'file': path.name, 'status': 'not_standard_hsd'}
        result = subprocess.run([str(out/'asset_probe'), str(path), '--validate-only'], capture_output=True, text=True)
        return {'file': path.name, 'status': 'failed' if result.returncode else 'passed', 'error': result.stderr}
    with ThreadPoolExecutor(max_workers=6) as pool:
        results = list(pool.map(check, paths))
    report = {key: sum(r['status'] == key for r in results) for key in ('passed', 'failed', 'not_standard_hsd')}
    report['files'] = results
    (out/'archive-audit.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f"HSD audit: {report['passed']} passed, {report['failed']} failed, {report['not_standard_hsd']} nonstandard", flush=True)
    if report['failed']:
        raise SystemExit(1)
    print(f"Menu textures decoded: {len(menu_report['textures'])}")
    print('MenMainBack frame-0 AnimJoint: 102 joints, 40 AObjs, 87 FObjs/channels, 0 unsupported')
if __name__ == '__main__':
    main()
