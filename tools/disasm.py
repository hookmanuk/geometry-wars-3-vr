"""Disassembles part of a memory dump made by memdump.py.
Usage: python dis.py <dump.bin> <base hex> <start hex> <length hex>"""
import capstone
import sys

d = open(sys.argv[1], 'rb').read()
base, start, length = (int(a, 16) for a in sys.argv[2:5])
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for ins in md.disasm(d[start - base:start - base + length], start):
    print('%08x  %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
