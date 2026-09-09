"""Read-only Vita file-call model; game DVD/archive code still executes in ARM."""
from pathlib import Path
import struct
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_PC, UC_ARM_REG_LR

def mount_disc(arm, root):
    root = Path(root).resolve()
    handles = {}
    opened = []
    prefix = 'ux0:data/SmashMeleeVita/files/'
    def path(address):
        name = arm.string(address).decode()
        assert name.startswith(prefix), name
        p = (root / name[len(prefix):]).resolve()
        assert p.is_relative_to(root)
        return p
    def service(m, address, size, name):
        a,b,c,d = [m.reg_read(r) for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
        result = 0
        if name == 'sceIoGetstat':
            p = path(a)
            if not p.is_file(): result = -1
            else:
                st = bytearray(0x58);struct.pack_into('<Q', st, 8, p.stat().st_size)
                m.mem_write(b, bytes(st))
        elif name == 'sceIoOpen':
            p = path(a)
            assert b == 1, 'only read-only access is modeled'
            if not p.is_file(): result = -1
            else:
                result = 100 + len(opened);opened.append(p.name)
                handles[result] = [p.read_bytes(), 0]
        elif name == 'sceIoRead':
            data,pos = handles[a]; chunk = data[pos:pos+c]
            m.mem_write(b, chunk);handles[a][1] += len(chunk);result = len(chunk)
        elif name == 'sceIoLseek':
            offset = (d << 32) | c
            if offset & (1 << 63): offset -= 1 << 64
            whence = struct.unpack('<I', m.mem_read(m.reg_read(UC_ARM_REG_SP),4))[0]
            data,pos = handles[a]
            result = (0 if whence == 0 else pos if whence == 1 else len(data)) + offset
            assert result >= 0
            handles[a][1] = result
            m.reg_write(UC_ARM_REG_R1, result >> 32)
        elif name == 'sceIoClose': del handles[a]
        m.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
        m.reg_write(UC_ARM_REG_PC, m.reg_read(UC_ARM_REG_LR))
    for name in ('sceIoGetstat','sceIoOpen','sceIoRead','sceIoLseek','sceIoClose'):
        address = arm.symbols[name] & ~1
        arm.uc.hook_add(UC_HOOK_CODE, service, user_data=name, begin=address, end=address)
    return opened
