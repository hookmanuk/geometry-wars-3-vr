"""Prints the constant table (names and registers) of dumped D3D9 shaders.

Usage: python ctab.py <folder of vs_*.bin / ps_*.bin> [hash ...]
"""
import glob
import os
import struct
import sys

REGSET = {0: 'b', 1: 'i', 2: 'c', 3: 's'}


def ctab(data):
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    i = 1
    while i < len(words):
        t = words[i]
        if t == 0x0000FFFF:
            break
        if (t & 0xFFFF) == 0xFFFE:
            n = (t >> 16) & 0x7FFF
            if words[i + 1] == 0x42415443:  # 'CTAB'
                base = (i + 2) * 4
                size, creator, version, count, info = struct.unpack_from('<5I', data, base)
                out = []
                for k in range(count):
                    name, regset, reg, regcount = struct.unpack_from('<IHHH', data, base + info + k * 20)
                    end = data.index(b'\0', base + name)
                    out.append((REGSET.get(regset, '?'), reg, regcount, data[base + name:end].decode()))
                return sorted(out)
            i += n + 1
        else:
            i += 1 + ((t >> 24) & 0xF)
    return None


def main():
    folder = sys.argv[1]
    wanted = sys.argv[2:]
    for path in sorted(glob.glob(os.path.join(folder, '*.bin'))):
        name = os.path.basename(path)[:-4]
        if wanted and not any(w in name for w in wanted):
            continue
        data = open(path, 'rb').read()
        ver = struct.unpack_from('<I', data)[0]
        table = ctab(data)
        print('%s  v%d.%d  %d bytes' % (name, (ver >> 8) & 0xFF, ver & 0xFF, len(data)))
        if table is None:
            print('   (no constant table)')
            continue
        for regset, reg, count, cname in table:
            print('   %s%-3d x%-2d %s' % (regset, reg, count, cname))


if __name__ == '__main__':
    main()
