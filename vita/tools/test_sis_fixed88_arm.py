#!/usr/bin/env python3
"""Check compiled ARM SIS unsigned 8.8 encoding used by dynamic text."""
import argparse, struct
from arm_harness import ArmHarness
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_S0, UC_ARM_REG_SP
p=argparse.ArgumentParser();p.add_argument("--elf",required=True);args=p.parse_args();arm=ArmHarness(args.elf)
def encode(value):
 sp=arm.stack+65536-256;arm.uc.reg_write(UC_ARM_REG_SP,sp);arm.uc.reg_write(UC_ARM_REG_LR,arm.stop);arm.uc.reg_write(UC_ARM_REG_S0,struct.unpack("<I",struct.pack("<f",value))[0]);target=arm.symbols["HSD_SisLib_VitaEncodeU8_8"];cpsr=arm.uc.reg_read(UC_ARM_REG_CPSR);arm.uc.reg_write(UC_ARM_REG_CPSR,(cpsr|0x20) if target&1 else (cpsr&~0x20));arm.uc.emu_start(target,arm.stop,timeout=30000000,count=30000000);assert arm.uc.reg_read(UC_ARM_REG_PC)==arm.stop;return arm.uc.reg_read(UC_ARM_REG_R0)&0xffff
for value,expected in ((0.0,0x0000),(0.5,0x0080),(1.0,0x0100),(1.25,0x0140),(2.0,0x0200)):
 actual=encode(value);assert actual==expected,(value,hex(actual),hex(expected))
print("PASS SIS fixed8.8 ARM: dynamic scales encode exactly")
