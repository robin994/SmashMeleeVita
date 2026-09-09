#!/usr/bin/env python3
"""Compile each upstream C unit for ARM independently; success is NOT game linkage."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from collections import Counter
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
args = parser.parse_args()
if args.jobs < 1:
    parser.error("--jobs must be positive")
sdk = Path(os.environ.get("VITASDK", "/usr/local/vitasdk"))
cc = sdk / "bin/arm-vita-eabi-gcc"
version = subprocess.check_output([str(cc), "--version"], text=True).splitlines()[0]
flags = ["-std=gnu11", "-O2", "-fmax-errors=3", "-Werror=implicit-function-declaration",
         "-Isrc", "-Iextern/dolphin/include", "-idirafter", "src/MSL"]
files = sorted([*ROOT.glob("src/**/*.c"), *ROOT.glob("extern/dolphin/src/**/*.c")])
out = ROOT / "build/vita/audit"
out.mkdir(parents=True, exist_ok=True)
def compile_one(path):
    relative = path.relative_to(ROOT)
    with tempfile.TemporaryDirectory(prefix="melee-vita-audit-") as temporary:
        result = subprocess.run([str(cc), *flags, "-c", str(relative), "-o", str(Path(temporary)/"unit.o")],
                                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = out / (str(relative).replace("/", "__") + ".log")
    log.write_text(result.stdout)
    errors = [line for line in result.stdout.splitlines() if "error:" in line]
    return {"source": str(relative), "compiled": result.returncode == 0,
            "first_error": errors[0] if errors else None, "log": str(log.relative_to(ROOT))}
with ThreadPoolExecutor(max_workers=args.jobs) as executor:
    results = list(executor.map(compile_one, files))
passed = sum(item["compiled"] for item in results)
report = {"upstream_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
          "compiler": version, "flags": flags, "total": len(results), "compiled": passed,
          "failed": len(results)-passed,
          "scope": "Independent object compilation only; no linking or runtime validation.", "units": results}
(out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
print(f"ARM object audit: {passed}/{len(results)} compiled; {len(results)-passed} failed")
print(out / "report.json")
for error, count in Counter((item["first_error"] or "unknown").split("error:")[-1].strip()
                            for item in results if not item["compiled"]).most_common(10):
    print(f"{count:4}  {error}")
