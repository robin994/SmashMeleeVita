"""Execute compiled ARM functions with modeled libc only, never Vita OS/GPU calls."""
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn.arm_const import (UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0,
    UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_PC,
    UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC)

class ArmHarness:
    def __init__(self, path):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        with open(path, 'rb') as stream:
            elf = ELFFile(stream)
            assert elf['e_machine'] == 'EM_ARM' and elf.little_endian
            for segment in elf.iter_segments():
                if segment['p_type'] != 'PT_LOAD':
                    continue
                address, size = segment['p_vaddr'], segment['p_memsz']
                base = address & ~4095
                self.uc.mem_map(base, (address + size - base + 4095) & ~4095)
                self.uc.mem_write(address, segment.data())
            self.symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            self.symbol_ranges = sorted((value & ~1, name) for name, value in self.symbols.items() if value)
        self.stop, self.stack, self.heap = 0x01000000, 0x02000000, 0x06000000
        # The Vita bring-up now models the GameCube's separate 16 MiB ARAM in
        # addition to the existing DAT/texture buffers. Keep this test-only
        # bump arena comfortably above the runtime's physical allocations.
        self.heap_end = self.heap + 96 * 1024 * 1024
        self.uc.mem_map(self.stop, 4096)
        self.uc.mem_map(self.stack, 65536)
        self.uc.mem_map(self.heap, self.heap_end-self.heap)
        self.uc.reg_write(UC_ARM_REG_C1_C0_2, 0xf << 20)
        self.uc.reg_write(UC_ARM_REG_FPEXC, 1 << 30)
        for name in ('memset', 'memcpy', 'memmove', 'malloc', 'calloc', 'free',
                     'strlen', 'strcmp', 'strstr', 'memchr'):
            if name in self.symbols:
                address = self.symbols[name] & ~1
                self.uc.hook_add(UC_HOOK_CODE, self.libc, user_data=name, begin=address, end=address)
        for name in ('sceCtrlSetSamplingMode', 'sceIoMkdir'):
            if name in self.symbols:
                address = self.symbols[name] & ~1
                self.uc.hook_add(UC_HOOK_CODE, self.vita_success, user_data=name,
                                 begin=address, end=address)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self.invalid_memory)

    def invalid_memory(self, machine, access, address, size, value, user_data):
        pc = machine.reg_read(UC_ARM_REG_PC) & ~1
        nearest = '<unknown>'
        for symbol_address, name in self.symbol_ranges:
            if symbol_address > pc:
                break
            nearest = f'{name}+0x{pc-symbol_address:x}'
        regs = [machine.reg_read(r) for r in
                (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_LR)]
        print(f'ARM invalid memory access={access} address=0x{address:08x} size={size} '
              f'pc=0x{pc:08x} ({nearest}) r0=0x{regs[0]:08x} r1=0x{regs[1]:08x} '
              f'r2=0x{regs[2]:08x} r3=0x{regs[3]:08x} lr=0x{regs[4]:08x}', flush=True)
        try:
            words = struct.unpack('<23I', machine.mem_read(regs[1], 92))
            print('ARM fault r1 words=' + ','.join(f'{word:08x}' for word in words), flush=True)
        except Exception:
            pass
        return False

    def alloc(self, length):
        address = self.heap
        self.heap += (length + 15) & ~15
        assert self.heap <= self.heap_end
        return address

    def string(self, address):
        data = bytearray()
        while len(data) < 65536:
            value = self.uc.mem_read(address + len(data), 1)[0]
            if not value:
                return bytes(data)
            data.append(value)
        raise AssertionError('unterminated string')

    def libc(self, machine, address, size, name):
        a, b, c = [machine.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)]
        result = a
        if name == 'memset':
            assert c <= 32 * 1024 * 1024
            if c: machine.mem_write(a, bytes([b & 255]) * c)
        elif name in ('memcpy', 'memmove'):
            assert c <= 32 * 1024 * 1024
            if c: machine.mem_write(a, bytes(machine.mem_read(b, c)))
        elif name in ('malloc', 'calloc'):
            length = a * b if name == 'calloc' else a
            result = self.alloc(length)
            if name == 'calloc' and length: machine.mem_write(result, bytes(length))
        elif name == 'free':
            result = 0  # bounded test heap is discarded after the process
        elif name == 'strlen': result = len(self.string(a))
        elif name == 'strcmp':
            aa, bb = self.string(a), self.string(b)
            result = (aa > bb) - (aa < bb)
        elif name == 'strstr':
            offset = self.string(a).find(self.string(b))
            result = 0 if offset < 0 else a + offset
        elif name == 'memchr':
            assert c <= 32 * 1024 * 1024
            offset = bytes(machine.mem_read(a, c)).find(bytes([b & 255])) if c else -1
            result = 0 if offset < 0 else a + offset
        machine.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    def vita_success(self, machine, address, size, name):
        machine.reg_write(UC_ARM_REG_R0, 0)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    def call(self, name, *args):
        sp = self.stack + 65536 - 256
        self.uc.reg_write(UC_ARM_REG_SP, sp)
        self.uc.reg_write(UC_ARM_REG_LR, self.stop)
        for register, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            self.uc.reg_write(register, value)
        if len(args) > 4:
            self.uc.mem_write(sp, struct.pack('<' + 'I' * (len(args)-4), *args[4:]))
        self.uc.emu_start(self.symbols[name], self.stop, timeout=30000000, count=30000000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == self.stop, f'{name}: did not return'
        return self.uc.reg_read(UC_ARM_REG_R0)
