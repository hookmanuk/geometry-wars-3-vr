"""Hides groups of vertex shaders in the running game (skip_vs in gw3vr.ini) and measures how
much a screen region changes, to find which shader draws something.

Usage: python -I skip_test.py <x0> <y0> <x1> <y1> <group> [<group> ...]
  region in full-resolution pixels of the side-by-side image; each group is vs hashes joined by '+'.
The settings file is restored at the end.
"""
import os
import shutil
import sys
import time

import numpy as np
from PIL import Image

GAME = r'H:/SteamLibrary/steamapps/common/Geometry Wars 3 - Dimensions'
INI = GAME + '/gw3vr.ini'
SCRATCH = r'H:/gw3-vr/scratch'


def shot(name):
    path = SCRATCH + '/' + name + '.bmp'
    if os.path.exists(path):
        os.remove(path)
    with open(GAME + '/gw3vr_shot.tmp', 'w') as f:
        f.write(path + ' full')
    os.replace(GAME + '/gw3vr_shot.tmp', GAME + '/gw3vr_shot.req')
    for _ in range(100):
        if os.path.exists(path):
            break
        time.sleep(0.1)
    time.sleep(0.5)
    im = np.asarray(Image.open(path).convert('RGB')).astype(np.int16)
    os.remove(path)
    return im


def main():
    x0, y0, x1, y1 = (int(v) for v in sys.argv[1:5])
    groups = sys.argv[5:]
    original = open(INI).read()
    backup = SCRATCH + '/ini_before_skip.ini'
    shutil.copy(INI, backup)
    try:
        base = shot('skip_base')[y0:y1, x0:x1]
        Image.fromarray(base.astype(np.uint8)).save(SCRATCH + '/skip_base.png')
        for g in groups:
            hashes = ','.join(g.split('+'))
            with open(INI, 'w') as f:
                f.write(original + '\nskip_vs=' + hashes + '\n')
            time.sleep(1.0)
            im = shot('skip')[y0:y1, x0:x1]
            diff = np.abs(im - base).sum(axis=2)
            changed = (diff > 60).sum()
            Image.fromarray(im.astype(np.uint8)).save(SCRATCH + '/skip_%s.png' % g.replace('+', '_')[:40])
            print('%-60s pixels changed: %d of %d (mean diff %.2f)' % (g, changed, diff.size, diff.mean()))
    finally:
        with open(INI, 'w') as f:
            f.write(original)
        print('settings restored')


if __name__ == '__main__':
    main()
