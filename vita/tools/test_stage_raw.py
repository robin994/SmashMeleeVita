#!/usr/bin/env python3
"""Run the linked ARM stage nativeizer on retail archives; no GPU emulation."""
import argparse, json, struct
from pathlib import Path
from arm_harness import ArmHarness
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_CPSR, UC_ARM_REG_SP
p=argparse.ArgumentParser();p.add_argument('--elf',default='build/vita-full/melee_vita');p.add_argument('--assets',default='orig/GALE01/files');p.add_argument('--only', nargs='*');p.add_argument('--output',default='build/vita-full/stage-raw-check.json');args=p.parse_args()
results=[]
for path in sorted(Path(args.assets).glob('Gr*.*')):
 if args.only and path.name not in args.only:continue
 raw=path.read_bytes()
 if len(raw)<32 or b'map_head\0' not in raw:continue
 arm=ArmHarness(args.elf);messages=[]
 def report(m,a,s,d):
  message=arm.string(m.reg_read(UC_ARM_REG_R0)).decode(errors='replace')
  if 'VITA_STAGE_HSD_RAW_VALIDATE_FAIL' in message:
   code,kind,offset,value=struct.unpack('<4I',m.mem_read(m.reg_read(UC_ARM_REG_SP),16))
   message=f'validate_fail map={m.reg_read(UC_ARM_REG_R2)} root={m.reg_read(UC_ARM_REG_R3):08x} code={code} kind={arm.string(kind).decode()} offset={offset:08x} value={value:08x}'
  messages.append(message)
  m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
 def panic(m,a,s,d):raise AssertionError(arm.string(m.reg_read(UC_ARM_REG_R2)).decode(errors='replace'))
 for name,hook in [('OSReport',report),('HSD_Panic',panic)]:
  addr=arm.symbols[name]&~1;arm.uc.hook_add(UC_HOOK_CODE,hook,begin=addr,end=addr)
 src=arm.alloc(len(raw));arm.uc.mem_write(src,raw);name=arm.alloc(len(path.name)+1);arm.uc.mem_write(name,path.name.encode()+b'\0')
 item={'file':path.name}
 try:
  try:
   arm.call('mv_stage_archive_prepare_raw',src,len(raw),name)
  except AssertionError as limit:
   if str(limit) != 'mv_stage_archive_prepare_raw: did not return':raise
   # Large stages exceed the small component harness's instruction budget.
   # Resume the same invocation; never restart a partly nativeized archive.
   for _ in range(8):
    pc=arm.uc.reg_read(UC_ARM_REG_PC)
    thumb=bool(arm.uc.reg_read(UC_ARM_REG_CPSR)&32)
    arm.uc.emu_start(pc|thumb,arm.stop,timeout=30000000,count=100000000)
    if arm.uc.reg_read(UC_ARM_REG_PC)==arm.stop:break
  assert arm.uc.reg_read(UC_ARM_REG_PC)==arm.stop,'nativeizer timed out'
  assert any('VITA_STAGE_HSD_RAW_NATIVE_PASS' in m for m in messages),'missing success marker'
  after=bytes(arm.uc.mem_read(src,len(raw)))
  size,nrel=struct.unpack_from('>II',raw,4)
  for i in range(nrel):
   off=32+struct.unpack_from('>I',raw,32+size+4*i)[0]
   assert raw[off:off+4]==after[off:off+4],f'relocation field changed {off-32:#x}'
  assert raw[:32]==after[:32] and raw[32+size:]==after[32+size:],'archive metadata changed'
  item['result']='PASS'
 except Exception as e:item.update(result='FAIL',error=str(e),messages=messages[-3:])
 results.append(item);print(json.dumps(item),flush=True)
Path(args.output).write_text(json.dumps(results,indent=2)+'\n')
raise SystemExit(any(r['result']!='PASS' for r in results))
