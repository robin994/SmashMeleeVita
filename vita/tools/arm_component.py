"""Bootstrap original HSD only, for renderer/animation tests (not a game boot test)."""
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_PC,UC_ARM_REG_LR

def boot_component(arm):
    def hook(m,address,size,name):
        if name in ('HSD_Panic','OSPanic'):
            raise AssertionError((name,arm.string(m.reg_read(UC_ARM_REG_R0)),m.reg_read(UC_ARM_REG_R1),arm.string(m.reg_read(UC_ARM_REG_R2))))
        m.reg_write(UC_ARM_REG_R0,0)
        if name=='sceKernelGetProcessTimeWide':m.reg_write(UC_ARM_REG_R1,0)
        m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
    for name in ('vfprintf','setvbuf','sceDisplayWaitVblankStart','HSD_Panic','OSPanic','sceKernelGetProcessTimeWide'):
        if name in arm.symbols:
            p=arm.symbols[name]&~1;arm.uc.hook_add(UC_HOOK_CODE,hook,user_data=name,begin=p,end=p)
    arm.call('OSInit');stats=arm.alloc(24)
    assert arm.call('HSD_InitComponentVitaProbe',stats)==0
    return stats
