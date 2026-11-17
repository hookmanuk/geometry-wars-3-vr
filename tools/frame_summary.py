"""Summarises a gw3vr_dump_N.log: one line per draw with its target, shaders and camera.

Usage: python frame_summary.py <dump.log>

The camera is taken from the last matrix written to c0-c3 and printed as
  fov <vertical degrees> near <n> far <f> origin-in-view (x y z) [flipY]
"""
import math
import re
import sys


def camera(rows):
    r = rows
    wl = math.sqrt(sum(v * v for v in r[3][:3]))
    lx = math.sqrt(sum(v * v for v in r[0][:3]))
    ly = math.sqrt(sum(v * v for v in r[1][:3]))
    if wl < 1e-6 or lx < 1e-9 or ly < 1e-9:
        return 'ortho'
    sy = ly / wl
    a = sum(r[2][i] * r[3][i] for i in range(3)) / (wl * wl)
    b = r[2][3] - a * r[3][3]
    near = -b / a / wl if a else 0
    far = -b / (a - 1) / wl if abs(a - 1) > 1e-9 else float('inf')
    origin = (r[0][3] / lx, r[1][3] / ly, r[3][3] / wl)
    flip = ' flipY' if r[1][1] < 0 else ''
    return 'fov %.1f near %.3g far %.5g origin (%.1f %.1f %.1f)%s' % (
        2 * math.degrees(math.atan(1 / sy)), near, far, origin[0], origin[1], origin[2], flip)


def main():
    cam = '-'
    vs = ps = target = '-'
    world = ''
    last = None
    count = 0
    for line in open(sys.argv[1]):
        if line.startswith('VSConst c0 x4'):
            nums = [float(x) for x in re.findall(r'-?\d+\.?\d*(?:e[-+]?\d+)?', line.split(':', 1)[1])]
            cam = camera([nums[i * 4:i * 4 + 4] for i in range(4)])
        elif line.startswith('VSConst c13 '):
            nums = [float(x) for x in re.findall(r'-?\d+\.?\d*(?:e[-+]?\d+)?', line.split(':', 1)[1])]
            if len(nums) >= 12:
                world = 'at (%.0f %.0f %.0f)' % (nums[3], nums[7], nums[11])
        elif line.startswith('SetVertexShader'):
            vs = line.split()[1]
        elif line.startswith('SetPixelShader'):
            ps = line.split()[1]
        elif line.startswith('SetRenderTarget'):
            m = re.search(r'(\d+x\d+) fmt (\d+)', line)
            target = m.group(1) if m else '?'
            print('--- target', target)
        elif line.startswith('Draw'):
            prims = re.search(r'prims (\d+)', line).group(1)
            key = (vs, ps, cam)
            if key == last:
                count += 1
                continue
            if count:
                print('      (+%d more)' % count)
            count = 0
            last = key
            print('%s %s prims %-5s %s  %s' % (vs, ps, prims, cam, world))
    if count:
        print('      (+%d more)' % count)


if __name__ == '__main__':
    main()
