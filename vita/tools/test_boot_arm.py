"""Exercise real HSD component init. Only libc I/O and the vblank wait are modeled."""
import struct
from unicorn.arm_const import UC_ARM_REG_PC
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_PC, UC_ARM_REG_LR

def boot_hsd(arm, final_init=True):
    traces = []
    waits = []
    thread_tls = arm.alloc(4096)
    def service(machine, address, size, name):
        if name == 'vfprintf':
            text = arm.string(machine.reg_read(UC_ARM_REG_R1)).decode()
            traces.append(text.strip())
            result = len(text)
        elif name == 'sceDisplayWaitVblankStart':
            waits.append(1)
            result = 0
        elif name == 'sceKernelGetProcessTimeWide':
            # AAPCS returns a 64-bit scalar in r0/r1. A deterministic clock is
            # sufficient here; hardware timing is validated separately on Vita.
            usec = 1_234_567
            machine.reg_write(UC_ARM_REG_R0, usec & 0xffffffff)
            machine.reg_write(UC_ARM_REG_R1, usec >> 32)
            machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))
            return
        elif name == 'sceIoGetstat':
            # Final game-state init probes /usa.ini to select the localized
            # language path. The pure ARM harness has no Vita filesystem, so
            # model a clean "not found" result (the normal US-disc fallback).
            result = -1
        elif name == 'sceKernelGetThreadId':
            result = 1
        elif name in ('sceKernelGetTLSAddr', 'sceKernelGetThreadTLSAddr'):
            # newlib's reentrancy layer asks Vita for a per-thread TLS slot
            # when original game code reaches libc helpers such as sprintf.
            # One zero-initialized deterministic slot is enough for this
            # single-threaded pure-ARM boot test.
            result = thread_tls
        elif name == 'sceKernelCreateMutex':
            result = 1
        elif name in ('sceKernelLockMutex', 'sceKernelUnlockMutex',
                      'sceKernelDeleteMutex'):
            result = 0
        else: # setvbuf only: standard stream buffering, not a game function
            result = 0
        machine.reg_write(UC_ARM_REG_R0, result)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))
    for name in ('vfprintf', 'setvbuf', 'sceDisplayWaitVblankStart',
                 'sceKernelGetProcessTimeWide', 'sceIoGetstat',
                 'sceKernelGetThreadId', 'sceKernelGetTLSAddr',
                 'sceKernelGetThreadTLSAddr', 'sceKernelCreateMutex',
                 'sceKernelLockMutex', 'sceKernelUnlockMutex',
                 'sceKernelDeleteMutex'):
        address = arm.symbols[name] & ~1
        arm.uc.hook_add(UC_HOOK_CODE, service, user_data=name, begin=address, end=address)
    arm.call('OSInit')
    stats_ptr = arm.alloc(24)
    assert arm.call('HSD_InitComponentVitaProbe', stats_ptr) == 0
    stats = struct.unpack('<6I', arm.uc.mem_read(stats_ptr, 24))
    assert stats[0] == 1 and stats[1] > 0 and stats[2] == 524256 and stats[4] == 25165824 and stats[5] == 0x3ff, stats
    expected = ['HSD_COMPONENT_BEGIN ' + name for name in
                ('OS', 'VI', 'GX', 'DVD', 'ID', 'RETRACE', 'OBJECTS', 'LOG', 'COMPLETE')]
    assert traces == expected, traces
    assert len(waits) == 1
    assert arm.call('mv_vi_boot_validate', stats_ptr) == 0
    video = struct.unpack('<6I', arm.uc.mem_read(stats_ptr, 24))
    assert video[0:4] == (640, 480, 2, 1) and video[5] == 255, video
    assert len(waits) == 2
    # Allocation must now use the original HSD memory API and configured OS heap.
    before = arm.call('OSCheckHeap', stats[0])
    pointer = arm.call('HSD_MemAlloc', 65)
    assert pointer and pointer % 32 == 0
    assert arm.call('OSCheckHeap', stats[0]) < before
    arm.call('HSD_Free', pointer)
    assert arm.call('OSCheckHeap', stats[0]) == before
    blocks = [arm.call('HSD_MemAlloc', n) for n in (65, 33, 97)]
    assert all(blocks) and len(set(blocks)) == 3
    for i in (1, 0, 2): arm.call('HSD_Free', blocks[i])
    assert arm.call('OSCheckHeap', stats[0]) == before
    assert arm.call('OSAllocFromHeap', stats[0], 0xffffffff) == 0
    post_ptr = arm.alloc(4)
    assert arm.call('gmMain_VitaPostHsdProbe', post_ptr) == 0
    seed_tick = struct.unpack('<I', arm.uc.mem_read(post_ptr, 4))[0]
    assert seed_tick != 0
    misc_ptr = arm.alloc(8)
    assert arm.call('mv_gx_misc_validate', misc_ptr) == 0
    misc = struct.unpack('<2I', arm.uc.mem_read(misc_ptr, 8))
    assert misc == (8, 0), misc
    audio_ptr = arm.alloc(32)
    assert arm.call('mv_audio_boot_validate', audio_ptr) == 0
    audio = struct.unpack('<8I', arm.uc.mem_read(audio_ptr, 32))
    assert audio[0:4] == (0x4000, 16 * 1024 * 1024, 0x1000, 0), audio
    assert all(audio[i] > 0 for i in range(4, 8)), audio
    assert audio[7] == audio[4] + audio[5] + audio[6], audio
    ax_ptr = arm.alloc(24)
    assert arm.call('mv_ax_boot_validate', ax_ptr) == 0
    ax = struct.unpack('<6I', arm.uc.mem_read(ax_ptr, 24))
    assert ax[0] == 64 and ax[1] == 0, ax
    assert ax[2] == 0 and ax[3] == 0, ax
    post_audio_ptr = arm.alloc(4)
    assert arm.call('gmMain_VitaPostAudioProbe', post_audio_ptr) == 0
    post_audio = struct.unpack('<I', arm.uc.mem_read(post_audio_ptr, 4))[0]
    assert post_audio == 0x7f, hex(post_audio)
    services_ptr = arm.alloc(4)
    try:
        services_result = arm.call('gmMain_VitaServicesProbe', services_ptr)
    except Exception as exc:
        services_stage = struct.unpack('<I', arm.uc.mem_read(services_ptr, 4))[0]
        pc = arm.uc.reg_read(UC_ARM_REG_PC) & ~1
        nearest = '<unknown>'
        for symbol_address, symbol_name in arm.symbol_ranges:
            if symbol_address > pc:
                break
            nearest = f'{symbol_name}+0x{pc-symbol_address:x}'
        raise AssertionError(
            f'gmMain services trapped after stage {services_stage:#x} at {pc:#x} ({nearest})') from exc
    assert services_result == 0
    services = struct.unpack('<I', arm.uc.mem_read(services_ptr, 4))[0]
    assert services == 0x3f, hex(services)
    if not final_init:
        print(f'ARM HSD_InitComponent: PASS stages=0x{stats[5]:x} main_free={stats[1]} (original call order)', flush=True)
        print('ARM VI/GX boot state: PASS black XFB, callback -> NEXT -> DISPLAY, 8 lights; vblank wait modeled', flush=True)
        print('ARM upstream HSD memory: PASS bounded OS heap, 32-byte alignment, free/coalesce, overflow rejection', flush=True)
        print(f'ARM gmmain audio core: PASS GXSetMisc=8, ARAM={audio[1]} bytes, '
              f'banks={audio[4]}/{audio[5]}/{audio[6]} total={audio[7]}', flush=True)
        print(f'ARM AXDriver/HSD_Synth init: PASS voices={ax[0]} allocated={ax[1]} '
              f'aux={ax[2]}/{ax[3]} mode={ax[4]} max_dsp={ax[5]} '
              '(stops before aux effects/bank allocation)', flush=True)
        print('ARM gmmain post-audio: PASS controller/retrace/VI/memory/heap/DVD/ARQ stages=0x7f',
              flush=True)
        print('ARM gmmain services: PASS cardnew/cardgame/snapshot/mainlib/MTHP/SisLib stages=0x3f',
              flush=True)
        return
    final_init_ptr = arm.alloc(4)
    try:
        final_init_result = arm.call('gmMain_VitaFinalInitProbe', final_init_ptr)
    except Exception as exc:
        pc = arm.uc.reg_read(UC_ARM_REG_PC) & ~1
        nearest = '<unknown>'
        for symbol_address, symbol_name in arm.symbol_ranges:
            if symbol_address > pc:
                break
            nearest = f'{symbol_name}+0x{pc-symbol_address:x}'
        recent_traces = ' | '.join(traces[-4:]) if traces else '<none>'
        raise AssertionError(
            f'gmMain final init trapped at {pc:#x} ({nearest}); '
            f'recent trace: {recent_traces}') from exc
    assert final_init_result == 0
    final_init = struct.unpack('<I', arm.uc.mem_read(final_init_ptr, 4))[0]
    assert final_init == 1, final_init
    gm_boot_ptr = arm.alloc(16)
    assert arm.call('gm_VitaBootStateProbe', gm_boot_ptr) == 0
    gm_boot = struct.unpack('<4I', arm.uc.mem_read(gm_boot_ptr, 16))
    assert gm_boot == (0x28, 0x2a, 0x18, 0), gm_boot
    gm_memcard_ptr = arm.alloc(16)
    assert arm.call('gm_VitaMemCardStateProbe', gm_memcard_ptr) == 0
    gm_memcard = struct.unpack('<4I', arm.uc.mem_read(gm_memcard_ptr, 16))
    assert gm_memcard == (0x2a, 1, 0, 0x18), gm_memcard
    print(f'ARM HSD_InitComponent: PASS stages=0x{stats[5]:x} main_free={stats[1]} (original call order)', flush=True)
    print('ARM VI/GX boot state: PASS black XFB, callback -> NEXT -> DISPLAY, 8 lights; vblank wait modeled', flush=True)
    print('ARM upstream HSD memory: PASS bounded OS heap, 32-byte alignment, free/coalesce, overflow rejection', flush=True)
    print(f'ARM gmmain audio core: PASS GXSetMisc=8, ARAM={audio[1]} bytes, '
          f'banks={audio[4]}/{audio[5]}/{audio[6]} total={audio[7]}', flush=True)
    print(f'ARM AXDriver/HSD_Synth init: PASS voices={ax[0]} allocated={ax[1]} '
          f'aux={ax[2]}/{ax[3]} mode={ax[4]} max_dsp={ax[5]} '
          '(stops before aux effects/bank allocation)', flush=True)
    print('ARM gmmain post-audio: PASS controller/retrace/VI/memory/heap/DVD/ARQ stages=0x7f',
          flush=True)
    print('ARM gmmain services: PASS cardnew/cardgame/snapshot/mainlib/MTHP/SisLib stages=0x3f',
          flush=True)
    print('ARM gmmain final init: PASS gmMainLib_8015FBA4 completed', flush=True)
    print('ARM GM_BOOT enter: PASS bootOnLoad -> GS_MEMCARD -> GM_OPENING_MV', flush=True)
    print('ARM GM_MEMCARD load: PASS memcardOnLoad initialized shared load_data; '
          'stops before gm_Scene_MemCard_OnEnter', flush=True)

if __name__ == '__main__':
    from arm_harness import ArmHarness
    boot_hsd(ArmHarness('build/vita/melee_vita'))
