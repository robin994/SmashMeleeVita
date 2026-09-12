#!/usr/bin/env python3
"""Verify Vita load-only HSD preserves scene-amnesia callbacks."""

import argparse
import struct

from arm_harness import ArmHarness


parser = argparse.ArgumentParser()
parser.add_argument("--elf", default="build/vita-full/melee_vita")
args = parser.parse_args()

arm = ArmHarness(args.elf)

# HSD_ClassInfoHead is 40 bytes on ARM, followed by
# alloc/init/release/destroy/amnesia function pointers.
AMNESIA_OFFSET = 56
checks = [
    ("JObjInfoInit", "hsdJObj", "JObjAmnesia"),
    ("DObjInfoInit", "hsdDObj", "DObjAmnesia"),
    ("MObjInfoInit", "hsdMObj", "MObjAmnesia"),
    ("PObjInfoInit", "hsdPObj", "PObjAmnesia"),
    ("TObjInfoInit", "hsdTObj", "TObjAmnesia"),
]

for init_name, info_name, callback_name in checks:
    missing = [
        name
        for name in (init_name, info_name, callback_name)
        if name not in arm.symbols
    ]
    assert not missing, f"missing symbols: {missing}"
    arm.call(init_name)
    actual = struct.unpack(
        "<I", arm.uc.mem_read(arm.symbols[info_name] + AMNESIA_OFFSET, 4)
    )[0]
    expected = arm.symbols[callback_name]
    assert (actual & ~1) == (expected & ~1), (
        init_name,
        hex(actual),
        hex(expected),
    )

# Reproduce the lifetime boundary behind the hardware crash. The scene heap
# owns shadow TObjs; hsdForgetClassLibrary() runs before that heap is destroyed.
# MObjAmnesia must clear these heads so the next fighter never traverses a
# recycled pointer chain.
shadow_head = arm.symbols["tobj_shadows"]
toon_head = arm.symbols["tobj_toon"]
arm.uc.mem_write(shadow_head, struct.pack("<I", 0x06001234))
arm.uc.mem_write(toon_head, struct.pack("<I", 0x06005678))

library = b"sysdolphin_base_library\0"
library_ptr = arm.alloc(len(library))
arm.uc.mem_write(library_ptr, library)
arm.call("hsdForgetClassLibrary", library_ptr)

assert struct.unpack("<I", arm.uc.mem_read(shadow_head, 4))[0] == 0
assert struct.unpack("<I", arm.uc.mem_read(toon_head, 4))[0] == 0

print(
    "PASS HSD scene amnesia: JObj/DObj/MObj/PObj/TObj callbacks registered; "
    "MObj shadow/toon heads cleared across class-library forget"
)
