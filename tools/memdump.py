"""Copies GW3.exe's loaded image out of the running game (the file on disk is encrypted by
Steam's DRM wrapper).  Usage: python memdump.py <out.bin>   Prints the image base."""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

k = ctypes.WinDLL('kernel32', use_last_error=True)
pid = int([l for l in subprocess.check_output(['tasklist', '/fi', 'imagename eq GW3.exe', '/fo', 'csv', '/nh']).decode().splitlines() if 'GW3' in l][0].split(',')[1].strip('"'))
h = k.OpenProcess(0x0410, False, pid)  # query information + vm read
psapi = ctypes.WinDLL('psapi')
mods = (wt.HMODULE * 512)()
need = wt.DWORD()
psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(need), 1)  # 32-bit modules
base = mods[0]


class MI(ctypes.Structure):
    _fields_ = [('base', ctypes.c_void_p), ('size', wt.DWORD), ('entry', ctypes.c_void_p)]


mi = MI()
psapi.GetModuleInformation(h, ctypes.c_void_p(base), ctypes.byref(mi), ctypes.sizeof(mi))
buf = ctypes.create_string_buffer(mi.size)
got = ctypes.c_size_t()
out = bytearray(mi.size)
page = 0x1000
for o in range(0, mi.size, page):
    if k.ReadProcessMemory(h, ctypes.c_void_p(mi.base + o), buf, page, ctypes.byref(got)):
        out[o:o + page] = buf.raw[:page]
open(sys.argv[1], 'wb').write(out)
print('pid %d base %#x size %#x' % (pid, mi.base, mi.size))
