#!/usr/bin/env python3
"""Pure ARM tests from the linked Vita ELF, with modeled libc and no OS/GPU."""
import argparse
from pathlib import Path
import json
import struct
from arm_harness import ArmHarness
from test_boot_arm import boot_hsd

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('elf', nargs='?', default='build/vita/melee_vita')
parser.add_argument('--assets', type=Path, help='MnMaAll.usd from the verified local disc')
parser.add_argument('--reference', type=Path, default=Path('build/vita/host/menu-textures'))
args = parser.parse_args()
arm = ArmHarness(args.elf)
boot_hsd(arm, final_init=False)
arm.uc.mem_write(arm.symbols['seed'], struct.pack('<I', 1))
values = [arm.call('HSD_Rand') for _ in range(5)]
assert values == [41, 51235, 6334, 59268, 51937], values
print('ARM HSD_Rand known-answer vectors: PASS', flush=True)
assert arm.call('melee_vita_pad_selftest') == 1
print('ARM Vita mapping + upstream PADClamp: PASS', flush=True)
if args.assets:
    source = args.assets.read_bytes()
    address = arm.alloc(len(source))
    arm.uc.mem_write(address, source)
    view = arm.alloc(128)
    assert arm.call('mv_dat_open', view, address, len(source)) == 0
    images, count_address = arm.alloc(2048 * 36), arm.alloc(4)
    assert arm.call('mv_menu_textures', view, images, 2048, count_address) == 0
    count = struct.unpack('<I', arm.uc.mem_read(count_address, 4))[0]
    reference = json.loads((args.reference/'report.json').read_text())['textures']
    assert count == len(reference), count
    records = [struct.unpack('<9I', arm.uc.mem_read(images + i*36, 36)) for i in range(count)]
    for record, expected in zip(records, reference):
        assert (record[0],record[6],record[7],record[8]) == (
            expected['image_desc'],expected['width'],expected['height'],expected['format'])
    root = b'MenMainBack_Top_joint\0'
    root_address = arm.alloc(len(root))
    arm.uc.mem_write(root_address, root)
    scene_stats_address = arm.alloc(10 * 4)
    assert arm.call('mv_scene_stats', view, root_address, scene_stats_address) == 0
    scene_stats = struct.unpack('<10I', arm.uc.mem_read(scene_stats_address, 40))
    scene_reference = json.loads((args.reference/'report.json').read_text())['scene']
    assert scene_reference is not None
    assert scene_stats[:7] == (
        scene_reference['meshes'], scene_reference['vertices'], scene_reference['triangles'],
        scene_reference['joints'], scene_reference['dobjs'], scene_reference['pobjs'],
        scene_reference['textured_meshes'])
    assert scene_stats[7:] == (
        scene_reference['skipped_pobjs'], scene_reference['skipped_primitives'],
        scene_reference['unsupported_joints'])
    print(f"ARM HSD geometry: PASS ({scene_stats[0]} meshes, {scene_stats[2]} triangles; no source mutation)", flush=True)
    anim_name = b'MenMainBack_Top_animjoint\0'
    anim_name_address = arm.alloc(len(anim_name))
    arm.uc.mem_write(anim_name_address, anim_name)
    anim_stats_address = arm.alloc(8 * 4)
    assert arm.call('mv_scene_frame0_stats', view, root_address, anim_name_address, anim_stats_address) == 0
    anim_stats = struct.unpack('<8I', arm.uc.mem_read(anim_stats_address, 32))
    anim_reference = json.loads((args.reference/'report.json').read_text())['animated_scene']
    assert anim_reference is not None
    assert anim_stats == (
        anim_reference['meshes'], anim_reference['triangles'], anim_reference['joints'],
        anim_reference['anim_joints'], anim_reference['aobjs'], anim_reference['fobjs'],
        anim_reference['channels'], anim_reference['unsupported'])
    root_anim_sample = arm.alloc(68)
    assert arm.call('mv_anim_joint_sample_frame0', view, 0x12C5C, root_anim_sample) == 0
    root_anim_raw = bytes(arm.uc.mem_read(root_anim_sample, 68))
    first_anim_offset = struct.unpack_from('<I', root_anim_raw, 0)[0]
    assert first_anim_offset == 0x12C70 and root_anim_raw[14] == 1
    first_anim_sample = arm.alloc(68)
    assert arm.call('mv_anim_joint_sample_frame0', view, first_anim_offset, first_anim_sample) == 0
    first_anim_raw = bytes(arm.uc.mem_read(first_anim_sample, 68))
    first_mask = struct.unpack_from('<H', first_anim_raw, 12)[0]
    first_channels = struct.unpack_from('<10f', first_anim_raw, 16)
    first_reference = json.loads((args.reference/'report.json').read_text())['first_anim_sample']
    expected_channels = tuple(first_reference['rot']) + (0.0,) + tuple(first_reference['translate'])
    assert first_mask == first_reference['mask']
    assert all(abs(a - b) < 1e-5 for a, b in zip(first_channels[:7], expected_channels)), (
        first_channels[:7], expected_channels)
    print(f"ARM HSD AnimJoint frame 0: PASS ({anim_stats[3]} joints, {anim_stats[5]} FObjs, {anim_stats[6]} channels)", flush=True)
    if 'mv_matanim_frame0_stats' in arm.symbols:
        matanim_name = b'MenMainBack_Top_matanim_joint\0'
        matanim_name_address = arm.alloc(len(matanim_name))
        arm.uc.mem_write(matanim_name_address, matanim_name)
        matanim_stats_address = arm.alloc(12 * 4)
        assert arm.call('mv_matanim_frame0_stats', view, root_address,
                        matanim_name_address, matanim_stats_address) == 0
        matanim_stats = struct.unpack('<12I', arm.uc.mem_read(matanim_stats_address, 48))
        matanim_reference = json.loads((args.reference/'report.json').read_text())['matanim']
        assert matanim_reference is not None
        assert matanim_stats == (
            matanim_reference['joints'], matanim_reference['matanims'], matanim_reference['texanims'],
            matanim_reference['aobjs'], matanim_reference['fobjs'], matanim_reference['channels'],
            matanim_reference['image_channels'], matanim_reference['uv_channels'],
            matanim_reference['blend_channels'], matanim_reference['transformed_meshes'],
            matanim_reference['multitexture_meshes'], matanim_reference['unsupported'])
        print(f"ARM HSD MatAnim frame 0: PASS ({matanim_stats[2]} TexAnim, "
              f"{matanim_stats[6]} image, {matanim_stats[7]} UV, {matanim_stats[8]} blend; "
              f"{matanim_stats[11]} unsupported)", flush=True)
    else:
        print('ARM HSD MatAnim frame 0: SKIP (release GC-sections removed test-only helper)',
              flush=True)
    camera_name = b'ScMenMain_cam_int1_camera\0'
    camera_name_address = arm.alloc(len(camera_name))
    arm.uc.mem_write(camera_name_address, camera_name)
    camera_address = arm.alloc(88)
    assert arm.call('mv_camera_read', view, camera_name_address, camera_address) == 0
    camera_raw = bytes(arm.uc.mem_read(camera_address, 88))
    camera_head = struct.unpack_from('<HHhhhhHHHH', camera_raw, 0)
    camera_floats = struct.unpack_from('<17f', camera_raw, 20)
    camera_reference = json.loads((args.reference/'report.json').read_text())['camera']
    assert camera_reference is not None
    assert camera_head[:2] == (0, camera_reference['type'])
    assert camera_head[2:6] == tuple(camera_reference['viewport'])
    expected = tuple(camera_reference['eye'] + camera_reference['interest'] + camera_reference['up'] +
                     [camera_reference['near'], camera_reference['far'], camera_reference['fov'],
                      camera_reference['aspect']])
    actual = camera_floats[:13]
    assert all(abs(a - b) < 1e-5 for a, b in zip(actual, expected)), (actual, expected)
    print(f"ARM HSD camera: PASS (perspective {actual[11]:.3f} deg, eye z={actual[2]:.1f})", flush=True)
    scene_address = arm.alloc(112)
    assert arm.call('mv_scene_build', view, root_address, scene_address) == 0
    visibility_address = arm.alloc(16)
    assert arm.call('mv_camera_visibility', camera_address, scene_address, visibility_address) == 0
    arm_visibility = struct.unpack('<4I', arm.uc.mem_read(visibility_address, 16))
    visibility_reference = json.loads((args.reference/'report.json').read_text())['visibility']
    assert arm_visibility == (visibility_reference['front'], visibility_reference['partial'],
                              visibility_reference['behind'], visibility_reference['far'])
    scene_bounds = struct.unpack('<6f', arm.uc.mem_read(scene_address + 84, 24))
    arm.call('mv_scene_free', scene_address)
    print(f"ARM HSD camera visibility: PASS ({arm_visibility[0]} front, {arm_visibility[1]} clipped)", flush=True)
    native_stats_address = arm.alloc(13 * 4)
    assert arm.call('mv_hsd_native_stats', view, root_address, native_stats_address) == 0
    native_stats = struct.unpack('<13I', arm.uc.mem_read(native_stats_address, 52))
    native_reference = json.loads((args.reference/'report.json').read_text())['native']
    assert native_reference is not None
    assert native_stats == (
        native_reference['joints'], native_reference['dobjs'], native_reference['mobjs'],
        native_reference['pobjs'], native_reference['tobjs'], native_reference['vtxdescs'],
        native_reference['images'], native_reference['tluts'], native_reference['materials'],
        native_reference['pedescs'], native_reference['lods'], native_reference['tevs'],
        native_reference['unsupported'])
    print(f"ARM HSD native descriptors: PASS ({native_stats[0]} joints, {native_stats[4]} TObjs, {native_stats[12]} unsupported)", flush=True)
    native_graph_address = arm.alloc(72)
    assert arm.call('mv_hsd_native_build', view, root_address, native_graph_address) == 0
    native_root = struct.unpack('<I', arm.uc.mem_read(native_graph_address, 4))[0]
    pending = [native_root]
    first_tobj_desc = 0
    seen_joints, seen_dobjs, seen_tobjs = set(), set(), set()
    while pending and not first_tobj_desc:
        joint = pending.pop()
        if not joint or joint in seen_joints:
            continue
        seen_joints.add(joint)
        child, nxt, dobj = struct.unpack('<III', arm.uc.mem_read(joint + 8, 12))
        if child: pending.append(child)
        if nxt: pending.append(nxt)
        while dobj and not first_tobj_desc:
            if dobj in seen_dobjs:
                break
            seen_dobjs.add(dobj)
            dobj_next, mobj = struct.unpack('<II', arm.uc.mem_read(dobj + 4, 8))
            if mobj:
                first_tobj_desc = struct.unpack('<I', arm.uc.mem_read(mobj + 8, 4))[0]
            dobj = dobj_next
    assert first_tobj_desc
    first_tobj_words = struct.unpack('<23I', arm.uc.mem_read(first_tobj_desc, 92))
    assert first_tobj_words[0] == 0
    assert first_tobj_words[19] != 0  # imagedesc at +0x4c
    print(f"ARM native TObj descriptor: first=0x{first_tobj_desc:08x} "
          f"next=0x{first_tobj_words[1]:08x} image=0x{first_tobj_words[19]:08x}", flush=True)
    # Validate every native TObj pointer field before handing the graph to the
    # upstream constructors. This catches a typed-conversion/layout bug without
    # relying on the constructor to dereference the bad pointer first.
    pending = [native_root]
    seen_joints.clear(); seen_dobjs.clear()
    while pending:
        joint = pending.pop()
        if not joint or joint in seen_joints: continue
        seen_joints.add(joint)
        child, nxt, dobj = struct.unpack('<III', arm.uc.mem_read(joint + 8, 12))
        if child: pending.append(child)
        if nxt: pending.append(nxt)
        while dobj:
            if dobj in seen_dobjs: break
            seen_dobjs.add(dobj)
            dobj_next, mobj = struct.unpack('<II', arm.uc.mem_read(dobj + 4, 8))
            if mobj:
                tobj = struct.unpack('<I', arm.uc.mem_read(mobj + 8, 4))[0]
                while tobj and tobj not in seen_tobjs:
                    seen_tobjs.add(tobj)
                    words = struct.unpack('<23I', arm.uc.mem_read(tobj, 92))
                    for pointer in (words[1], words[19], words[20], words[21], words[22]):
                        assert pointer == 0 or 0x06000000 <= pointer < 0x0a000000, (
                            hex(tobj), [hex(x) for x in (words[1], words[19], words[20], words[21], words[22])])
                    tobj = words[1]
            dobj = dobj_next
    assert len(seen_tobjs) == native_reference['tobjs'], len(seen_tobjs)
    print(f"ARM native TObj pointer graph: PASS ({len(seen_tobjs)} descriptors)", flush=True)
    arm.call('HSD_IDInitAllocData')
    arm.call('HSD_IDSetup')
    runtime_root = arm.call('HSD_JObjLoadJoint', native_root)
    assert runtime_root != 0
    print("ARM upstream HSD_JObjLoadJoint: PASS (live runtime graph created)", flush=True)
    capture_stats_address = arm.alloc(12 * 4)
    assert arm.call('mv_hsd_gx_capture_runtime', runtime_root, 1, 0,
                    capture_stats_address) == 0
    capture_stats = struct.unpack('<12I', arm.uc.mem_read(capture_stats_address, 48))
    assert capture_stats[0] == native_reference['pobjs'], capture_stats
    assert capture_stats[1] > 0 and capture_stats[2] > 0
    assert capture_stats[3] == scene_reference['triangles'], capture_stats
    assert capture_stats[11] == 0, capture_stats
    print(f"ARM upstream HSD PObj GX capture: PASS ({capture_stats[0]} lists, "
          f"{capture_stats[1]} commands, {capture_stats[2]} vertices, "
          f"{capture_stats[3]} triangles)", flush=True)
    replay_stats_address = arm.alloc(9 * 4)
    assert arm.call('mv_gx_capture_replay_classify', replay_stats_address) == 0
    replay_stats = struct.unpack('<9I', arm.uc.mem_read(replay_stats_address, 36))
    assert sum(replay_stats[4:]) + replay_stats[0] == capture_stats[1], replay_stats
    assert replay_stats == (capture_stats[1], capture_stats[3], capture_stats[1], 0,
                            0, 0, 0, 0, 0), replay_stats
    print(f"ARM GX replay subset: PASS ({replay_stats[0]} commands / "
          f"{replay_stats[1]} triangles supported; textured={replay_stats[2]}, "
          f"untextured={replay_stats[3]}, multitex={replay_stats[4]}, "
          f"tev={replay_stats[5]}, texcoord={replay_stats[6]}, "
          f"mapmode={replay_stats[7]}, vtxcolor={replay_stats[8]})", flush=True)
    if 'mv_gx_capture_pe_stats' in arm.symbols:
        pe_stats_address = arm.alloc(12 * 4)
        assert arm.call('mv_gx_capture_pe_stats', pe_stats_address) == 0
        pe_stats = struct.unpack('<12I', arm.uc.mem_read(pe_stats_address, 48))
        assert pe_stats == (96, 81, 15, 0, 96, 0, 96, 15, 8, 0, 88, 0), pe_stats
        print('ARM GX PE state: commands=%u alpha=%u additive=%u other=%u '
              'z_lequal=%u z_write=%u alpha_always=%u custom=%u '
              'cull_none=%u front=%u back=%u all=%u' % pe_stats, flush=True)
    if 'mv_gx_capture_custom_tev_records' in arm.symbols:
        tev_records_address = arm.alloc(10 * 9 * 4)
        tev_record_count = arm.call('mv_gx_capture_custom_tev_records', tev_records_address, 10)
        assert tev_record_count == 10, (tev_record_count, replay_stats)
    if 'mv_gx_capture_multitex_records' in arm.symbols:
        multitex_records_address = arm.alloc(4 * 28 * 4)
        multitex_record_count = arm.call('mv_gx_capture_multitex_records', multitex_records_address, 4)
        assert multitex_record_count == 1, (multitex_record_count, replay_stats)
    if 'mv_gx_capture_world_bounds' in arm.symbols:
        capture_bounds_address = arm.alloc(6 * 4)
        assert arm.call('mv_gx_capture_world_bounds', capture_bounds_address) == 0
        capture_bounds = struct.unpack('<6f', arm.uc.mem_read(capture_bounds_address, 24))
        assert all(abs(a - b) <= 0.01 for a, b in zip(capture_bounds, scene_bounds)), (
            capture_bounds, scene_bounds)
        print("ARM GX rigid world bounds: PASS (capture matches materialized scene within 0.01)",
              flush=True)
    arm.call('mv_hsd_native_free', native_graph_address)

    for extra_root_name, extra_anim_name, extra_matanim_name in (
        (b'MenMainPanel_Top_joint\0', b'MenMainPanel_Top_animjoint\0',
         b'MenMainPanel_Top_matanim_joint\0'),
        (b'MenMainConTop_Top_joint\0', b'MenMainConTop_Top_animjoint\0',
         b'MenMainConTop_Top_matanim_joint\0'),
        (b'MenMainCursor_Top_joint\0', b'MenMainCursor_Top_animjoint\0',
         b'MenMainCursor_Top_matanim_joint\0'),
    ):
        extra_root_address = arm.alloc(len(extra_root_name))
        arm.uc.mem_write(extra_root_address, extra_root_name)
        extra_graph = arm.alloc(72)
        assert arm.call('mv_hsd_native_build', view, extra_root_address, extra_graph) == 0
        extra_native_root = struct.unpack('<I', arm.uc.mem_read(extra_graph, 4))[0]
        arm.call('HSD_IDInitAllocData')
        arm.call('HSD_IDSetup')
        extra_runtime_root = arm.call('HSD_JObjLoadJoint', extra_native_root)
        assert extra_runtime_root != 0

        extra_anim_name_address = arm.alloc(len(extra_anim_name))
        arm.uc.mem_write(extra_anim_name_address, extra_anim_name)
        extra_anim_graph = arm.alloc(32)
        assert arm.call('mv_native_anim_build', view, extra_anim_name_address,
                        extra_anim_graph) == 0
        extra_anim_root = struct.unpack('<I', arm.uc.mem_read(extra_anim_graph, 4))[0]

        extra_matanim_name_address = arm.alloc(len(extra_matanim_name))
        arm.uc.mem_write(extra_matanim_name_address, extra_matanim_name)
        extra_matanim_graph = arm.alloc(36)
        assert arm.call('mv_native_matanim_build', view, extra_matanim_name_address,
                        extra_matanim_graph) == 0
        extra_matanim_root = struct.unpack('<I', arm.uc.mem_read(extra_matanim_graph, 4))[0]

        arm.call('HSD_JObjAddAnimAll', extra_runtime_root, extra_anim_root,
                 extra_matanim_root, 0)
        arm.call('HSD_JObjReqAnimAll', extra_runtime_root, 0)
        arm.call('HSD_JObjAnimAll', extra_runtime_root)
        arm.call('HSD_JObjSetupMatrixSub', extra_runtime_root)
        print(f"ARM menu native live animation: PASS ({extra_root_name[:-1].decode()})",
              flush=True)

        extra_capture = arm.alloc(12 * 4)
        extra_capture_result = arm.call('mv_hsd_gx_capture_runtime', extra_runtime_root, 1, 0,
                                        extra_capture)
        print(f"ARM menu native GX capture probe: {extra_root_name[:-1].decode()} "
              f"result={extra_capture_result}", flush=True)
        assert extra_capture_result == 0
        extra_capture_stats = struct.unpack('<12I', arm.uc.mem_read(extra_capture, 48))
        assert extra_capture_stats[0] > 0 and extra_capture_stats[1] > 0
        assert extra_capture_stats[11] == 0, extra_capture_stats
        print(f"ARM menu native GX capture: PASS ({extra_root_name[:-1].decode()}, "
              f"{extra_capture_stats[0]} lists, {extra_capture_stats[3]} triangles)",
              flush=True)
        arm.call('mv_native_matanim_free', extra_matanim_graph)
        arm.call('mv_native_anim_free', extra_anim_graph)
        arm.call('mv_hsd_native_free', extra_graph)
    data_size, reloc_count, public_count, extern_count = struct.unpack_from('>4I', source, 4)
    public_base = 32 + data_size + reloc_count * 4
    strings_base = public_base + (public_count + extern_count) * 8
    expected_public_offset = None
    for i in range(public_count):
        public_offset, name_offset = struct.unpack_from('>II', source, public_base + i * 8)
        end = source.index(0, strings_base + name_offset)
        if source[strings_base + name_offset:end] == root[:-1]:
            expected_public_offset = public_offset
            break
    assert expected_public_offset is not None
    if 'mv_hsd_archive_probe' in arm.symbols:
        archive_copy = arm.alloc(len(source))
        arm.uc.mem_write(archive_copy, source)
        archive_stats_address = arm.alloc(6 * 4)
        assert arm.call('mv_hsd_archive_probe', archive_copy, len(source), root_address,
                        archive_stats_address) == 0
        archive_stats = struct.unpack('<6I', arm.uc.mem_read(archive_stats_address, 24))
        assert archive_stats == (len(source), data_size, reloc_count, public_count,
                                 extern_count, expected_public_offset), archive_stats
        print(f"ARM upstream HSD_ArchiveParse: PASS ({archive_stats[2]} relocations, "
              f"{archive_stats[3]} public roots, root=0x{archive_stats[5]:x})", flush=True)
    else:
        print('ARM upstream HSD_ArchiveParse: SKIP (release GC-sections removed test-only helper)',
              flush=True)
    assert bytes(arm.uc.mem_read(address, len(source))) == source
    print(f'ARM HSD typed traversal: PASS ({count} descriptors; source unchanged)', flush=True)
    selected = {}
    for i, record in enumerate(records):
        selected.setdefault(record[8], i)
    output = arm.alloc(4 * 1024 * 1024)
    for fmt, i in sorted(selected.items()):
        record = records[i]
        length, stride = record[6] * record[7] * 4, record[6] * 4
        assert arm.call('mv_image_decode', view, images + i*36, output, length, stride) == 0
        expected = (args.reference / f'{i:04}.rgba').read_bytes()
        assert bytes(arm.uc.mem_read(output, length)) == expected, (fmt, i)
        print(f'ARM GX{fmt} texture {i}: byte parity PASS', flush=True)
    arm.call('mv_dat_close', view)
print('Scope: libc is modeled; real Vita display/input and gameplay remain unverified.')
