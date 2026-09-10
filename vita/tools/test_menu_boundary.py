#!/usr/bin/env python3
"""ARM regression: scene exit ownership and retail Event menu flag conversion.
No Vita input, GPU output, full menu traversal or gameplay is modeled here.
"""
from pathlib import Path
import struct
from arm_harness import ArmHarness
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_LR

arm = ArmHarness('build/vita/melee_vita')
def quiet(machine, address, size, data):
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))
for name in ('OSReport',):
    address = arm.symbols[name] & ~1
    arm.uc.hook_add(UC_HOOK_CODE, quiet, begin=address, end=address)
def panic(machine, address, size, data):
    raise AssertionError('Event adapter rejected an archive descriptor')
address = arm.symbols['HSD_Panic'] & ~1
arm.uc.hook_add(UC_HOOK_CODE, panic, begin=address, end=address)
def word(address):
    return struct.unpack('<I', arm.uc.mem_read(address, 4))[0]
def alloc(data):
    address = arm.alloc(len(data))
    arm.uc.mem_write(address, data)
    return address

arm.call('mv_scene_vita_reset')
title = arm.call('gm_GetCurrentSceneExitData')
arm.uc.mem_write(title, struct.pack('<I', 0x12345678))
assert arm.call('mv_scene_vita_pending_mode') == 45
arm.call('mv_scene_vita_begin_menu')
menu = arm.call('gm_GetCurrentSceneExitData')
assert menu != title
assert arm.call('mv_scene_vita_pending_mode') == 45
for mode in range(45):
    arm.uc.mem_write(menu, bytes([mode, 0, 0, 0]))
    arm.call('gm_801A4B60')
    assert arm.call('mv_scene_vita_done') == 1
    assert arm.call('mv_scene_vita_pending_mode') == mode
    assert word(title) == 0
    arm.call('mv_scene_vita_begin_menu')
    assert arm.call('mv_scene_vita_done') == 0
    assert arm.call('mv_scene_vita_pending_mode') == 45
arm.call('mv_scene_vita_reset')
assert arm.call('gm_GetCurrentSceneExitData') == title
print('PASS: separate title/menu storage, all 45 GM IDs, reset invalidation')

raw = Path('orig/GALE01/files/GmEvent.dat').read_bytes()
src, archive = alloc(raw), arm.alloc(0x44)
assert arm.call('HSD_ArchiveParse', archive, src, len(raw)) == 0
levels = arm.call('HSD_ArchiveGetPublicAddress', archive,
                  alloc(b'sqEventInitDataLevelTbl\0'))
expected = {}
for i in range(51):
    level = word(levels + 4*i)
    init = word(level + 8)
    a, b = arm.uc.mem_read(init, 2)
    expected[init] = (((a >> 5) | (((a >> 2) & 7) << 3) |
                      (((a >> 1) & 1) << 6) | ((a & 1) << 7)),
                     ((b >> 7) | (((b >> 6) & 1) << 1) |
                      (((b >> 5) & 1) << 2) | (((b >> 4) & 1) << 3) |
                      (((b >> 3) & 1) << 4) | ((b & 7) << 5)))
before = bytes(arm.uc.mem_read(src, len(raw)))
arm.call('mv_event_menu_archive_prepare', archive)
after = bytes(arm.uc.mem_read(src, len(raw)))
allowed = set()
for init, flags in expected.items():
    assert bytes(arm.uc.mem_read(init, 2)) == bytes(flags)
    allowed.update((init-src, init-src+1))
assert all(a == b or i in allowed for i, (a, b) in enumerate(zip(before, after)))
# Bad relocated pointers must be rejected before a dereference.
arm.uc.mem_write(levels, struct.pack('<I', src - 4))
try:
    arm.call('mv_event_menu_archive_prepare', archive)
except AssertionError as exc:
    assert 'rejected an archive descriptor' in str(exc)
else:
    raise AssertionError('invalid Event pointer accepted')
print(f'PASS: 51 retail Event entries, {len(expected)} unique init flags, unrelated bytes preserved, malformed pointer rejected')
