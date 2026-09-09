"""Exercise real HSD component init. Only libc I/O and the vblank wait are modeled."""
import struct
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_PC, UC_ARM_REG_LR

def boot_hsd(arm):
    from arm_disc import mount_disc
    opened = mount_disc(arm, "orig/GALE01/files")
    traces = []
    waits = []
    def service(machine, address, size, name):
        if name in ('HSD_Panic', 'OSPanic'):
            from unicorn.arm_const import UC_ARM_REG_R2
            raise AssertionError(f'{name}: {arm.string(machine.reg_read(UC_ARM_REG_R0))!r}:{machine.reg_read(UC_ARM_REG_R1)} {arm.string(machine.reg_read(UC_ARM_REG_R2))!r}')
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
        else: # setvbuf only: standard stream buffering, not a game function
            result = 0
        machine.reg_write(UC_ARM_REG_R0, result)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))
    for name in ('vfprintf', 'setvbuf', 'sceDisplayWaitVblankStart',
                 'sceKernelGetProcessTimeWide', 'HSD_Panic', 'OSPanic'):
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
    gm_boot_ptr = arm.alloc(16)
    assert arm.call('gm_VitaBootStateProbe', gm_boot_ptr) == 0
    gm_boot = struct.unpack('<4I', arm.uc.mem_read(gm_boot_ptr, 16))
    assert gm_boot == (0x28, 0x2a, 0x18, 0), gm_boot
    memcard_ptr = arm.alloc(32)
    try:
        memcard_result = arm.call('gm_VitaMemCardSceneEnterProbe', memcard_ptr)
    except Exception as exc:
        stage = struct.unpack('<I', arm.uc.mem_read(memcard_ptr, 4))[0]
        pc = arm.uc.reg_read(UC_ARM_REG_PC) & ~1
        nearest = '<unknown>'
        for symbol_address, symbol_name in arm.symbol_ranges:
            if symbol_address > pc:
                break
            nearest = f'{symbol_name}+0x{pc-symbol_address:x}'
        raise AssertionError(
            f'GS_MEMCARD enter trapped after stage {stage:#x} at {pc:#x} ({nearest})') from exc
    assert memcard_result == 0
    memcard = struct.unpack('<8I', arm.uc.mem_read(memcard_ptr, 32))
    assert memcard[0:2] == (7, 2) and memcard[2] in (1, 2, 3), memcard
    assert memcard[3:5] == (640, 480) and memcard[5] > 0, memcard
    assert memcard[6:8] == (0, 0x18), memcard
    assert opened == ['LbMcGame.usd', 'NtMemAc.usd', 'NtMsgWin.dat', 'SdMsgBox.usd'], opened
    # Independent comparison with the original big-endian camera descriptor.
    from pathlib import Path
    import math
    data = Path('orig/GALE01/files/NtMsgWin.dat').read_bytes()
    _, data_size, reloc, publics = struct.unpack_from('>4I', data)
    public_base = 32 + data_size + 4*reloc
    externs = struct.unpack_from('>I', data, 16)[0]
    symbols = public_base + 8*(publics + externs)
    roots = {}
    for i in range(publics):
        off, name = struct.unpack_from('>2I', data, public_base+8*i)
        end = data.index(b'\0', symbols+name)
        roots[data[symbols+name:end].decode()] = 32+off
    def beptr(off): return 32+struct.unpack_from('>I', data, off)[0]
    desc = beptr(beptr(roots['ScNtcCommon_scene_data']+4))
    eye = struct.unpack_from('>3f', data, beptr(desc+24)+4)
    interest = struct.unpack_from('>3f', data, beptr(desc+28)+4)
    def word(p): return struct.unpack('<I', arm.uc.mem_read(p,4))[0]
    entities = word(arm.symbols['HSD_GObj_Entities'])
    camera_gobj = word(entities+21*4)
    camera = word(camera_gobj+0x28)
    native_eye = struct.unpack('<3f', arm.uc.mem_read(word(camera+0x24)+0x0c,12))
    # WObj position follows the HSD_Obj header and flags (offset 0x0c).
    assert native_eye == eye, (native_eye, eye)
    near_far = struct.unpack('<2f', arm.uc.mem_read(camera+0x38,8))
    assert near_far == struct.unpack_from('>2f',data,desc+0x28)
    def normalize(v):
        mag = math.sqrt(sum(x*x for x in v));return [x/mag for x in v]
    def cross(a,b): return [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
    look = normalize([a-b for a,b in zip(eye,interest)])
    right = normalize(cross([0,1,0],look));up = cross(look,right)
    expected_view = [x for axis in (right,up,look) for x in (*axis,-sum(a*b for a,b in zip(eye,axis)))]
    actual_view = struct.unpack('<12f',arm.uc.mem_read(camera+0x54,48))
    assert all(abs(a-b)<1e-4 for a,b in zip(actual_view,expected_view)), (actual_view,expected_view)
    print('ARM original scene camera: PASS original disc eye/near/far and independent look-at matrix',flush=True)
    print('ARM GS_MEMCARD scene stats:', memcard, 'files:', opened, flush=True)
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
    print('ARM GM_BOOT enter: PASS bootOnLoad -> GS_MEMCARD -> GM_OPENING_MV', flush=True)
    print('ARM GS_MEMCARD enter: PASS original OnEnter + archive/UI setup', flush=True)

if __name__ == '__main__':
    from arm_harness import ArmHarness
    boot_hsd(ArmHarness('build/vita/melee_vita'))
