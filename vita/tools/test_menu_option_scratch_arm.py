#!/usr/bin/env python3
"""Guard ARM menu scratch capacity against Melee menu selection counts."""

from arm_harness import ArmHarness

arm = ArmHarness("build/vita-full/melee_vita")
capacity = arm.call("mn_VitaOptionScratchCapacity")
table = arm.symbols["mn_803EB6B0"]

# MenuKindData is 0x14 bytes on ARM32; selection_count is byte +0x0C.
counts = [arm.uc.mem_read(table + i * 0x14 + 0x0C, 1)[0] for i in range(0x22)]
assert counts[4] == 6, f"MENU_KIND_SETTINGS expected 6 entries, got {counts[4]}"
assert max(counts) == 10, f"expected retail max menu count 10, got {max(counts)}"
assert capacity >= max(counts), (capacity, max(counts), counts)
assert capacity == 12, capacity
print(f"PASS Vita menu option scratch: capacity={capacity} settings={counts[4]} retail_max={max(counts)}; no ARM stack overwrite from 6/10-entry menus")
