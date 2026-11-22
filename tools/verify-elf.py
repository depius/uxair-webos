#!/usr/bin/env python3
"""Reject a mismatched ARM ABI or dependencies newer than this TV's libc/runtime.

This is a preflight, not proof that the proprietary media APIs work. The on-TV
loader check and playback probe are also required (see docs/VALIDATION.md).
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
READELF = ROOT / '.cache/arm-webos-linux-gnueabi_sdk-buildroot/bin/arm-webos-linux-gnueabi-readelf'

def output(*args):
    return subprocess.check_output([str(READELF), *map(str,args)],text=True)

def version(v):
    return tuple(map(int, v.split('.')))

def main():
    stage = pathlib.Path(sys.argv[1])
    binary = sys.argv[2] if len(sys.argv) > 2 else 'uxair'
    if binary not in ('uxair', 'hevc-probe', 'direct-uri-probe'):
        sys.exit('Unsupported binary name')
    files = [stage / binary, stage / 'lib/libstdc++.so.6']
    direct = binary == 'direct-uri-probe' or (binary == 'uxair' and (stage / 'direct-uri-probe').is_file())
    exported = output('-V', files[1]).split('Version needs section')[0]
    max_glibc = (0, 0)
    for file in files:
        header = output('-h', file)
        if 'ELF32' not in header or not re.search(r'Machine:\s+ARM\b', header) or 'soft-float ABI' not in header:
            sys.exit(f'Wrong target ABI: {file}')
        versions = output('-V',file).split('Version needs section')[-1]
        for v in re.findall(r'Name: GLIBC_(\d+(?:\.\d+)+)', versions):
            if version(v)>version('2.19'):
                sys.exit(f'{file}: requires GLIBC_{v}, newer than TV 2.19')
            max_glibc = max(max_glibc, version(v))
        for name in re.findall(r'Name: ((?:GLIBCXX|CXXABI)[_A-Z.0-9]+)',versions):
            if name not in exported:
                sys.exit(f'{file}: bundled C++ runtime lacks {name}')
        print(f'{file.name}:')
        print(output('-A', file))
        print(output('-d', file))
    dynamic = output('-d',files[0])
    marker = ROOT / 'build/media-backend.txt'
    if binary == 'uxair' and marker.exists() and marker.read_text().strip() == 'starfish':
        if 'libNDL' in dynamic or 'libndl' in dynamic:
            sys.exit('Starfish package unexpectedly depends on NDL')
    if direct:
        if max_glibc > (2, 7):
            sys.exit('Direct Video must retain the v1.3.0 Starfish GLIBC_2.7 launch baseline')
        versions = output('-V', files[0]).split('Version needs section')[-1]
        for required in re.findall(r'Name: GLIBCXX_([0-9.]+)', versions):
            if version(required) > (3, 4, 29):
                sys.exit(f'Direct Video adds a mandatory GLIBCXX_{required} launch dependency')
        symbols = output('--dyn-syms', '--wide', files[0])
        for name in ('16notifyForegroundEv', '18getCurrentPlaytimeEv', '4SeekEPKc'):
            symbol = '_ZN17StarfishMediaAPIs' + name
            imports = [line.split() for line in symbols.splitlines() if symbol in line.split()]
            if len(imports) != 1 or imports[0][4] != 'WEAK' or imports[0][6] != 'UND':
                sys.exit(f'{symbol} must remain an optional weak native import')
        if '[Requesting program interpreter: /lib/ld-linux.so.3]' not in output('-l', files[0]):
            sys.exit('Direct Video changed the v1.3.0 native launcher interpreter')
        print('Direct Video launch ABI: GLIBC <= 2.7, GLIBCXX <= 3.4.29, optional Starfish URI symbols.')
    if '$ORIGIN/lib' not in dynamic:
        sys.exit('Missing private runtime search path')
    for bad in ['libcrypto.so', 'libssl.so', 'libcurl.so', 'libplist', 'libavcodec', 'libavformat', 'libgstreamer', 'libX11']:
        if bad in dynamic:
            sys.exit(f'Unexpected dynamic dependency: {bad}')
    print(f'ELF preflight: ARM ELF32 soft-float ABI; max required GLIBC_{".".join(map(str, max_glibc))} <= 2.19; private C++ runtime resolves version requirements.')

if __name__ == '__main__':
    main()
