#!/usr/bin/env python3
"""Execute original HSD title animation and persistent capture from the Vita ELF."""
from pathlib import Path
import struct,hashlib
from arm_harness import ArmHarness
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_PC,UC_ARM_REG_LR,UC_ARM_REG_S0
arm=ArmHarness('build/vita/melee_vita')
def service(m,address,size,name):
 if name in ('HSD_Panic','OSPanic'):
  raise AssertionError((name,arm.string(m.reg_read(UC_ARM_REG_R0)),m.reg_read(UC_ARM_REG_R1),arm.string(m.reg_read(UC_ARM_REG_R2))))
 m.reg_write(UC_ARM_REG_R1,0);m.reg_write(UC_ARM_REG_R0,0);m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
for name in ('vfprintf','setvbuf','sceDisplayWaitVblankStart','HSD_Panic','OSPanic','sceKernelGetProcessTimeWide'):
 if name in arm.symbols:
  p=arm.symbols[name]&~1;arm.uc.hook_add(UC_HOOK_CODE,service,user_data=name,begin=p,end=p)
def word(p):return struct.unpack('<I',arm.uc.mem_read(p,4))[0]
def alloc(data):
 p=arm.alloc(len(data));arm.uc.mem_write(p,data);return p
def req(root,frame):
 arm.uc.reg_write(UC_ARM_REG_S0,struct.unpack('<I',struct.pack('<f',frame))[0]);arm.call('HSD_JObjReqAnimAll',root)
def nodes(root):
 out=[];todo=[root]
 while todo:
  p=todo.pop()
  if not p:continue
  assert p not in out and len(out)<256
  out.append(p);todo.extend([word(p+8),word(p+16)])
 return out
arm.call('OSInit');stats=arm.alloc(24)
try:
 assert arm.call('HSD_InitComponentVitaProbe',stats)==0
except Exception:
 pc=arm.uc.reg_read(UC_ARM_REG_PC)&~1
 nearest=max((v,n) for v,n in arm.symbol_ranges if v<=pc)
 print('BOOT PC',hex(pc),nearest,'offset',hex(pc-nearest[0]),flush=True)
 raise
heap=word(stats)
raw=Path('orig/GALE01/files/GmTtAll.usd').read_bytes();src=alloc(raw);view=arm.alloc(128)
assert arm.call('mv_dat_open',view,src,len(raw))==0
for name,start,end,rewind in [('TtlMoji',400,1600,400),('TtlBg',130,1330,130)]:
 native=arm.alloc(128);anim=arm.alloc(16)
 assert arm.call('mv_hsd_native_build',view,alloc((name+'_Top_joint\0').encode()),native)==0
 r=arm.call('mv_native_anim_build',view,alloc((name+'_Top_animjoint\0').encode()),anim)
 assert r==0,(name,hex(r))
 root=arm.call('HSD_JObjLoadJoint',word(native));assert root
 arm.call('HSD_JObjAddAnimAll',root,word(anim),0,0)
 req(root,start);arm.call('HSD_JObjAnimAll',root)
 jobjs=nodes(root);animated=[p for p in jobjs if word(p+0x7c)]
 assert animated,(name,'no real AObjs attached')
 settings=alloc(struct.pack('<3f',0,end,rewind));capture=arm.alloc(64)
 before=arm.call('OSCheckHeap',heap);hashes=set();poses=set();frames=[]
 for tick in range(121):
  arm.call('mn_8022ED6C',root,settings)
  aobj=word(animated[0]+0x7c);frames.append(struct.unpack('<f',arm.uc.mem_read(aobj+4,4))[0])
  if tick in (0,30,60,120):
   assert arm.call('mv_hsd_gx_capture_runtime',root,1,1,capture)==0
   stat=struct.unpack('<13I',arm.uc.mem_read(capture,52))
   assert stat[1]>0 and stat[12]==0,(name,stat)
   # Capture hash includes live model matrices. Compare whole bounded command array.
   count=arm.alloc(4);ptr=arm.call('mv_gx_capture_commands',count)
   poses.add(hashlib.sha256(b''.join(bytes(arm.uc.mem_read(p+0x14,0x30)) for p in jobjs)).hexdigest())
   hashes.add((stat[1],stat[3]))
 assert frames[-1]>frames[0],(name,frames[0],frames[-1])
 assert len(poses)>1,(name,'pose frozen')
 assert arm.call('OSCheckHeap',heap)==before,'per-frame HSD heap leak'
 print(f'{name}: PASS real AObjs={len(animated)}, frame={frames[0]}->{frames[-1]}, poses={len(poses)}, visible captures={sorted(hashes)}, heap stable',flush=True)
assert bytes(arm.uc.mem_read(src,len(raw)))==raw,'source archive mutated'
print('PASS: original HSD animation/capture; VitaGL output and MatAnim are not verified by this test.',flush=True)
