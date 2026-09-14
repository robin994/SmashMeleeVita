#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
DIRECT_FIFO = re.compile(r"\bGXWGFifo\.[A-Za-z0-9_]+\s*=")

hits = []
for tree in ("src", "vita"):
    for path in (ROOT / tree).rglob("*"):
        if path.suffix not in {".c", ".h"}:
            continue
        for lineno, line in enumerate(
            path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
        ):
            if DIRECT_FIFO.search(line):
                hits.append(f"{path.relative_to(ROOT)}:{lineno}:{line.strip()}")

if hits:
    print("FAIL direct GameCube GX write-gather FIFO stores remain:")
    print("\n".join(hits))
    sys.exit(1)

print("PASS GX FIFO MMIO: no direct GXWGFifo stores remain in Vita-compiled source")
