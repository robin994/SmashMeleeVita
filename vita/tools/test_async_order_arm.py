#!/usr/bin/env python3
"""Regression for GameCube-visible DVD/DevCom asynchronous completion ordering."""

from pathlib import Path

from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R2,
)

DVD_STATE_END = 0
DVD_STATE_BUSY = 1

arm = ArmHarness("build/vita-full/melee_vita")
boot_component(arm)
arm.call("DVDInit")


def aligned_alloc(size, align=32):
    raw = arm.alloc(size + align - 1)
    return (raw + align - 1) & ~(align - 1)


# Model only the physical storage operation. The code under test is the Vita
# async wrapper, original HSD DevCom, and their completion pump.
def dvd_read(machine, _address, _size, _data):
    length = machine.reg_read(UC_ARM_REG_R2)
    machine.reg_write(UC_ARM_REG_R0, length)
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


read_addr = arm.symbols["DVDReadPrio"] & ~1
arm.uc.hook_add(UC_HOOK_CODE, dvd_read, begin=read_addr, end=read_addr)


# Direct DVD API: host bytes may be ready immediately, but guest state must be
# BUSY and the request must remain queued until a later scheduling boundary.
def async_pending():
    return arm.call("mv_gc_async_pending")


file_info = aligned_alloc(0x100)
buffer = aligned_alloc(64)
arm.uc.mem_write(file_info, bytes(0x100))
arm.uc.mem_write(buffer, bytes(64))

accepted = arm.call("DVDReadAsyncPrio", file_info, buffer, 32, 0, 0, 2)
assert accepted == 1
assert async_pending() == 1
assert arm.call("DVDGetDriveStatus") == DVD_STATE_BUSY
assert async_pending() == 0
assert arm.call("DVDGetDriveStatus") == DVD_STATE_END


# Critical cross-device phase: real SFX DevCom transfers alternate DVD and ARQ.
# Once the DVD callback has queued only ARQ work, Melee keeps calling
# lb_800195D0 -> DVDGetDriveStatus while waiting for HSD_Synth completion. That
# scheduling boundary must advance ARQ too, even though the DVD drive is idle.
ar_stack = aligned_alloc(16 * 4)
arm.uc.mem_write(ar_stack, bytes(16 * 4))
assert arm.call("ARInit", ar_stack, 16) != 0
arm.call("ARQInit")
arq_request = aligned_alloc(0x20)
arq_source = aligned_alloc(32)
arm.uc.mem_write(arq_request, bytes(0x20))
arm.uc.mem_write(arq_source, bytes(range(32)))
arq_dest = arm.call("ARAlloc", 32)
assert arq_dest != 0
arm.call("ARQPostRequest", arq_request, 0x4D56, 0, 0, arq_source, arq_dest, 32, 0)
assert async_pending() == 1
assert arm.call("DVDGetDriveStatus") == DVD_STATE_END
assert async_pending() == 0


# Retail lbArq synchronous wait regression from the v3.81 hardware freeze.
# Unicorn cannot execute Vita import stubs, so model sceKernelDelayThread as an
# immediate successful yield while leaving the actual alarm/async pumps intact.
def delay_thread(machine, _address, _size, _data):
    machine.reg_write(UC_ARM_REG_R0, 0)
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


delay_addr = arm.symbols["sceKernelDelayThread"] & ~1
arm.uc.hook_add(UC_HOOK_CODE, delay_thread, begin=delay_addr, end=delay_addr)

# lbArq_80014BD0(callback=NULL) posts a deferred ARAM->MRAM completion and then
# polls its node. On GameCube the ARQ interrupt advances that node; on Vita the
# blocking loop must pump the cooperative completion itself.
arm.call("lbArq_80014D2C")
sync_source = aligned_alloc(32)
arm.uc.mem_write(sync_source, bytes((0x80 + i) & 0xFF for i in range(32)))
sync_aram = arm.call("ARAlloc", 32)
assert sync_aram != 0
sync_req = aligned_alloc(0x20)
arm.uc.mem_write(sync_req, bytes(0x20))
arm.call("ARQPostRequest", sync_req, 0x4C425441, 0, 0, sync_source, sync_aram, 32, 0)
assert async_pending() == 1
arm.call("mv_gc_async_pump")
assert async_pending() == 0
sync_dest = aligned_alloc(32)
arm.uc.mem_write(sync_dest, bytes(32))
arm.call("lbArq_80014BD0", sync_aram, sync_dest, 32, 0, 0)
assert async_pending() == 0
assert bytes(arm.uc.mem_read(sync_dest, 32)) == bytes((0x80 + i) & 0xFF for i in range(32))


# Original HSD DevCom must sit above that same boundary rather than being a
# Vita synchronous replacement. DVDFastOpen only needs to report a valid file
# for this ordering test; physical reads remain mocked by DVDReadPrio above.
def fast_open(machine, _address, _size, _data):
    info = machine.reg_read(UC_ARM_REG_R1)
    arm.uc.mem_write(info, bytes(0x100))
    machine.reg_write(UC_ARM_REG_R0, 1)
    machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))


fast_open_addr = arm.symbols["DVDFastOpen"] & ~1
arm.uc.hook_add(UC_HOOK_CODE, fast_open, begin=fast_open_addr, end=fast_open_addr)

devcom_dest = aligned_alloc(64)
arm.uc.mem_write(devcom_dest, bytes(64))

req = arm.call("HSD_DevComRequest", 1, 0, devcom_dest, 32, 0x21, 0, 0, 0)
assert req >= 0
assert async_pending() == 1

# The first query observes the queued request as BUSY and then advances one
# emulated interrupt. The second query observes the completed request.
assert arm.call("HSD_DevComIsBusy", req & 3) == 1
assert async_pending() == 0
assert arm.call("HSD_DevComIsBusy", req & 3) == 0


# Vita recovery regression: emulate the impossible state observed on hardware
# where DevCom still owns a request and its DVD-active latch is set, but the
# cooperative host completion queue is empty.  A busy poll must clear/restart
# that stale latch, submit a fresh DVD completion, pump it, and retire the
# original DevCom request instead of leaving every later preload in state=2.
def map_bss_address(section_name):
    lines = Path("build/vita-full/melee_vita.map").read_text().splitlines()
    needle = ".bss." + section_name
    for i, line in enumerate(lines[:-1]):
        if needle in line:
            return int(lines[i + 1].split()[0], 16)
    raise AssertionError("missing map section " + needle)


stale_req = arm.call("HSD_DevComRequest", 1, 0, devcom_dest, 32, 0x21, 0, 0, 0)
assert stale_req >= 0
assert async_pending() == 1
active_latch = map_bss_address("HSD_DevCom_804D77F5")
dvd_pending_count = map_bss_address("dvd_pending_count")
dvd_pending_head = map_bss_address("dvd_pending_head")
dvd_pending_tail = map_bss_address("dvd_pending_tail")
assert arm.uc.mem_read(active_latch, 1)[0] == 1
# Drop the queued completion completely, not merely its visible count.  Leaving
# head behind would let a later test dispatch the synthetic lost IRQ as a ghost.
tail = int.from_bytes(arm.uc.mem_read(dvd_pending_tail, 4), "little")
arm.uc.mem_write(dvd_pending_head, tail.to_bytes(4, "little"))
arm.uc.mem_write(dvd_pending_count, (0).to_bytes(4, "little"))
assert async_pending() == 0
assert arm.call("HSD_DevComIsBusy", stale_req & 3) == 1
assert async_pending() == 0
assert arm.call("HSD_DevComIsBusy", stale_req & 3) == 0

# Re-entrant DevCom regression from the v3.75 hardware coredump.  A
# relay-buffer completion may enqueue the next direct-memory leg from inside
# its callback.  The executing completion must still count as active until the
# callback returns, otherwise DVDWakeUp treats its latch as stale and replaces
# the global dvdDC underneath the outer callback.
reentrant_req = arm.call("mv_devcom_reentrant_probe_begin")
assert reentrant_req >= 0
assert async_pending() == 1
arm.call("mv_gc_async_pump")
assert arm.call("mv_devcom_reentrant_probe_state") == 1
assert async_pending() == 1
arm.call("mv_gc_async_pump")
assert arm.call("mv_devcom_reentrant_probe_state") == 3
assert async_pending() == 0

print(
    "PASS GC async ordering: DVD and ARQ completions are deferred; ARQ-only DevCom legs advance "
    "through the retail DVD status scheduling boundary and synchronous lbArq waits without deadlock"
)
