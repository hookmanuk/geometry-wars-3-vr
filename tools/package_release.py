"""Builds the release zip from out/ and dist/.  Usage: python tools/package_release.py <version>

Writes release/gw3-vr-v<version>.zip with the files a player copies into the game folder, plus
the licences and the README. Run build.cmd first.
"""
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    version = sys.argv[1]
    header = open(os.path.join(ROOT, 'src', 'common.h')).read()
    built = re.search(r'#define GW3VR_VERSION "([^"]+)"', header).group(1)
    if built != version:
        sys.exit('src/common.h says version %s, not %s' % (built, version))
    files = {
        'd3d9.dll': os.path.join(ROOT, 'out', 'd3d9.dll'),
        'openxr_loader.dll': os.path.join(ROOT, 'out', 'openxr_loader.dll'),
        'gw3vr.ini': os.path.join(ROOT, 'dist', 'gw3vr.ini'),
        'README.md': os.path.join(ROOT, 'README.md'),
        'LICENSE.txt': os.path.join(ROOT, 'LICENSE'),
        'openxr_loader-LICENSE.txt': os.path.join(ROOT, 'out', 'openxr_loader-LICENSE.txt'),
    }
    dll = files['d3d9.dll']
    sources = [os.path.join(ROOT, 'src', f) for f in os.listdir(os.path.join(ROOT, 'src'))]
    if max(os.path.getmtime(s) for s in sources) > os.path.getmtime(dll):
        sys.exit('out/d3d9.dll is older than the sources: run build.cmd first')
    os.makedirs(os.path.join(ROOT, 'release'), exist_ok=True)
    path = os.path.join(ROOT, 'release', 'gw3-vr-v%s.zip' % version)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, src in files.items():
            z.write(src, name)
    print(path)
    for info in zipfile.ZipFile(path).infolist():
        print('  %-28s %9d' % (info.filename, info.file_size))


if __name__ == '__main__':
    main()
