#!/usr/bin/env python3
"""Regression for the v3.40 GrKg map-1 spline rejection, against linked ARM."""
from pathlib import Path
import struct
from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_LR
arm=ArmHarness('build/vita-full/melee_vita')
def silent(m,a,s,d):m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
a=arm.symbols['OSReport']&~1;arm.uc.hook_add(UC_HOOK_CODE,silent,begin=a,end=a)
boot_component(arm)
def alloc(data):
 p=arm.alloc(len(data));arm.uc.mem_write(p,data);return p
def word(p):return struct.unpack('<I',arm.uc.mem_read(p,4))[0]
raw=Path('orig/GALE01/files/GrKg.dat').read_bytes();src=alloc(raw);view=arm.alloc(128);probe=arm.alloc(256)
assert arm.call('mv_dat_open',view,src,len(raw))==0
assert struct.unpack_from('>I',raw,32+0x3b414)[0]==0x4008
assert arm.call('mv_hsd_native_build_at',view,0x3afe8,probe)==1,'strict proxy must still reject spline'
arm.call('mv_hsd_native_free',probe)
assert arm.call('mv_hsd_native_validate_raw_at',view,0x3afe8,probe)==0
arm.call('mv_hsd_native_free',probe)
# Corrupt only the referenced spline's numcv. Raw validation must reject it.
spline=struct.unpack_from('>I',raw,32+0x3b420)[0]
arm.uc.mem_write(src+32+spline+2,b'\0\0')
assert arm.call('mv_hsd_native_validate_raw_at',view,0x3afe8,probe)!=0
arm.call('mv_hsd_native_free',probe)
arm.uc.mem_write(src+32+spline+2,raw[32+spline+2:32+spline+4])
arm.call('mv_stage_archive_prepare_raw',src,len(raw),alloc(b'GrKg.dat\0'))
assert word(src+32+0x3b414)==0x4008
assert struct.unpack('<H',arm.uc.mem_read(src+32+spline+2,2))[0]==struct.unpack_from('>H',raw,32+spline+2)[0]
archive=arm.alloc(0x44)
assert arm.call('HSD_ArchiveParse',archive,src,len(raw))==0
root=arm.call('HSD_JObjLoadJoint',src+32+0x3afe8)
assert root
arm.call('HSD_JObjRemoveAll',root)
print('PASS GrKg map 1: strict proxy rejects; raw validator accepts; malformed spline rejected; nativeization, relocation, original HSD load and removal pass.')

# GrSt map 2 is a retail linear spline. Unlike curved spline types, type 0
# never reads segPoly in splArcLengthGetParameter(), and this asset correctly
# stores a NULL pointer there. The raw validator/converter must accept that
# shape while still converting cv/segLength and loading it through real HSD.
raw=Path('orig/GALE01/files/GrSt.dat').read_bytes();src=alloc(raw);view=arm.alloc(128);probe=arm.alloc(256)
assert arm.call('mv_dat_open',view,src,len(raw))==0
grst_root=0x18388
grst_spline_joint=0x18530
grst_spline=struct.unpack_from('>I',raw,32+grst_spline_joint+0x10)[0]
assert struct.unpack_from('>I',raw,32+grst_spline_joint+4)[0]&0x4000
assert raw[32+grst_spline] == 0
assert struct.unpack_from('>I',raw,32+grst_spline+0x14)[0] == 0
assert arm.call('mv_hsd_native_validate_raw_at',view,grst_root,probe)==0
arm.call('mv_hsd_native_free',probe)
arm.call('mv_stage_archive_prepare_raw',src,len(raw),alloc(b'GrSt.dat\0'))
assert struct.unpack('<H',arm.uc.mem_read(src+32+grst_spline+2,2))[0]==struct.unpack_from('>H',raw,32+grst_spline+2)[0]
archive=arm.alloc(0x44)
assert arm.call('HSD_ArchiveParse',archive,src,len(raw))==0
root=arm.call('HSD_JObjLoadJoint',src+32+grst_root)
assert root
arm.call('HSD_JObjRemoveAll',root)
print('PASS GrSt map 2: linear spline with NULL segPoly nativeizes, relocates, loads and removes through original HSD.')
