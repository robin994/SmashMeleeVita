#!/usr/bin/env python3
"""ARM regression for the GrNSr retail POBJ_SHAPEANIM stage graph."""
from pathlib import Path
import struct
from arm_harness import ArmHarness
from arm_component import boot_component
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_LR
arm=ArmHarness('build/vita-full/melee_vita')
def silent(m,a,s,d):m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
a=arm.symbols['OSReport']&~1;arm.uc.hook_add(UC_HOOK_CODE,silent,begin=a,end=a)
boot_component(arm)
def alloc(data):
 p=arm.alloc(len(data));arm.uc.mem_write(p,data);return p
raw=Path('orig/GALE01/files/GrNSr.dat').read_bytes();src=alloc(raw)
root=0x490a8;pobj=0x46194;shape=struct.unpack_from('>I',raw,32+pobj+0x14)[0]
assert struct.unpack_from('>H',raw,32+pobj+0x0c)[0]==0x9000
assert struct.unpack_from('>H',raw,32+shape+2)[0]==3
assert struct.unpack_from('>I',raw,32+shape+4)[0]==28
arm.call('mv_stage_archive_prepare_raw',src,len(raw),alloc(b'GrNSr.dat\0'))
assert struct.unpack('<H',arm.uc.mem_read(src+32+pobj+0x0c,2))[0]==0x9000
assert struct.unpack('<H',arm.uc.mem_read(src+32+shape+2,2))[0]==3
assert struct.unpack('<I',arm.uc.mem_read(src+32+shape+4,4))[0]==28
archive=arm.alloc(0x44)
assert arm.call('HSD_ArchiveParse',archive,src,len(raw))==0
jobj=arm.call('HSD_JObjLoadJoint',src+32+root)
assert jobj
arm.call('HSD_JObjRemoveAll',jobj)
print('PASS GrNSr map 4: retail POBJ_SHAPEANIM ShapeSet nativeizes, relocates and loads through original HSD.')
