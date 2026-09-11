#!/usr/bin/env python3
"""Regression for PPC .data adjacency assumptions in Player_80036E20."""

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2


arm = ArmHarness("build/vita-full/melee_vita")
boot_component(arm)

calls = []

def capture_ftdemo(machine, _address, _size, _data):
    calls.append((
        machine.reg_read(UC_ARM_REG_R0),
        machine.reg_read(UC_ARM_REG_R1),
        machine.reg_read(UC_ARM_REG_R2),
    ))
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

hook = arm.symbols["ftDemo_SetArchiveData"] & ~1
arm.uc.hook_add(UC_HOOK_CODE, capture_ftdemo, begin=hook, end=hook)

# CharacterKind -> expected FighterKind calls.  These cover a normal fighter,
# Ice Climbers' two-model mapping, and both transform pairs where the second
# mapping must not be loaded by Player_80036E20.
cases = [
    (8, [0]),       # Mario -> Ft_Kind_Mario
    (2, [1]),       # Fox -> Ft_Kind_Fox
    (14, [10, 11]), # PopoNana -> Popo + Nana
    (18, [19]),     # Zelda -> Zelda; Sheik is a transform, not a second demo
    (19, [7]),      # Sheik -> Sheik; Zelda is a transform
]

archive = 0x12345678
arr_idx = 1
for ckind, expected in cases:
    calls.clear()
    arm.call("Player_80036E20", ckind, archive, arr_idx)
    got = [x[0] for x in calls]
    assert got == expected, f"ckind {ckind}: mapping {got}, expected {expected}"
    assert all(x[1] == archive and x[2] == arr_idx for x in calls)

print("PASS player mapping: Player_80036E20 uses ftMapping_list, independent of PPC .data adjacency")
