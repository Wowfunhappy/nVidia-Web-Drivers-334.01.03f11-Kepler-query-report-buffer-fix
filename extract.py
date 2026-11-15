#!/usr/bin/env python3
"""Extract the __text bytes of an assembled object and disassemble them at
a given address.  usage: extract.py OBJ OUT.bin ADDRESS"""
import struct, sys, capstone
obj, out, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
d = open(obj, 'rb').read()
ncmds = struct.unpack_from('<I', d, 16)[0]; p = 32
code = None
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', d, p)
    if cmd == 0x19:
        for s in range(struct.unpack_from('<I', d, p + 64)[0]):
            sp = p + 72 + 80 * s
            if d[sp:sp+16].split(b'\0')[0] == b'__text':
                size_, off = struct.unpack_from('<QI', d, sp + 40)
                nreloc = struct.unpack_from('<I', d, sp + 60)[0]
                code = d[off:off + size_]
                if nreloc: sys.exit('object still has %d relocations' % nreloc)
    p += size
open(out, 'wb').write(code)
print('%d bytes, no relocations' % len(code))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for ins in md.disasm(code, base):
    print('  %#x: %-22s %s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
