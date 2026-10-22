#!/usr/bin/env python3
"""Check that the packaged IPK contains the expected app and no local state."""
import argparse
import io
import json
import pathlib
import re
import tarfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


def fail(message):
    raise SystemExit(f'Invalid IPK: {message}')


def ar_members(path):
    data = path.read_bytes()
    if not data.startswith(b'!<arch>\n'):
        fail('not an ar/IPK archive')
    offset = 8
    result = {}
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:] != b'`\n':
            fail('malformed ar header')
        name = header[:16].decode('ascii').strip().rstrip('/')
        try:
            size = int(header[48:58].decode('ascii').strip())
        except ValueError:
            fail('malformed ar member size')
        offset += 60
        if offset + size > len(data) or name in result:
            fail('truncated or duplicate ar member')
        result[name] = data[offset:offset + size]
        offset += size + size % 2
    if offset != len(data):
        fail('trailing ar data')
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--development', action='store_true')
    args = parser.parse_args()
    app = json.loads((ROOT / 'packaging/appinfo.json').read_text())
    if args.development:
        app['id'] += '.dev'
        app['title'] += ' Dev'
        app['version'] = '1.2.0'
    stage = ROOT / 'build/app'
    staged = json.loads((stage / 'appinfo.json').read_text())
    if staged != app:
        fail('staged appinfo does not match requested package identity')
    package = ROOT / 'dist' / f"{app['id']}_{app['version']}_arm.ipk"
    if not package.is_file():
        fail(f'missing {package.name}')
    members = ar_members(package)
    payloads = [value for name, value in members.items() if name.startswith('data.tar')]
    if len(payloads) != 1:
        fail('expected exactly one data.tar payload')
    with tarfile.open(fileobj=io.BytesIO(payloads[0]), mode='r:*') as archive:
        files = {member.name.lstrip('./'): member for member in archive if member.isfile()}
        def content(suffix):
            names = [name for name in files if name == suffix or name.endswith('/' + suffix)]
            if len(names) != 1:
                fail(f'expected exactly one {suffix} in package')
            return archive.extractfile(files[names[0]]).read()
        packaged = json.loads(content('appinfo.json'))
        if packaged != app:
            fail('packaged appinfo differs from staged metadata')
        if content('icon.png') != (ROOT / 'packaging/icon.png').read_bytes():
            fail('packaged launcher icon differs from the current UxAir artwork')
        if content('largeIcon.png') != (ROOT / 'packaging/largeIcon.png').read_bytes():
            fail('packaged large launcher icon differs from the current UxAir artwork')
        if not content('uxair') or not content('lib/libstdc++.so.6'):
            fail('missing receiver or private C++ runtime')
        if args.development:
            probe = content('direct-uri-probe')
            if probe[:6] != b'\x7fELF\x01\x01' or int.from_bytes(probe[18:20], 'little') != 40 or not int.from_bytes(probe[36:40], 'little') & 0x200:
                fail('native URI probe must be ARM ELF32 little-endian soft-float')
        elif any(path.endswith('/direct-uri-probe') for path in files):
            fail('development probe in production package')
        qr = content('bmc-qr.bmp')
        if qr != (ROOT / 'assets/bmc-qr.bmp').read_bytes() or qr[:2] != b'BM':
            fail('bundled donation QR differs from the committed local image')
        if int.from_bytes(qr[18:22], 'little') != 407 or int.from_bytes(qr[22:26], 'little', signed=True) != 407:
            fail('donation QR must be the expected 407x407 bitmap')
        if content('hls-ca.pem') != (ROOT / 'assets/hls-ca.pem').read_bytes():
            fail('HLS CA trust bundle differs from the committed trust store')
        coffee = content('bmc-icon.bmp')
        if coffee != (ROOT / 'assets/bmc-icon.bmp').read_bytes() or coffee[:2] != b'BM':
            fail('bundled Buy Me a Coffee icon differs from the supplied artwork')
        if int.from_bytes(coffee[18:22], 'little') != 120 or int.from_bytes(coffee[22:26], 'little', signed=True) != 120:
            fail('coffee icon must be the optimized square 120x120 bitmap')
        build_info = content('build-info.txt').decode('ascii')
        if not re.fullmatch(r'version=[^\n]+\ncommit=[0-9a-f]{40}\n', build_info):
            fail('missing valid source commit metadata')
        if build_info != (stage / 'build-info.txt').read_text():
            fail('packaged build metadata differs from staged source commit')
        forbidden = {'paired.keys', 'server.key', 'device.id', 'uxplay.log', 'uxair.log', 'audio-delay-ms'}
        if any('state' in pathlib.PurePosixPath(name).parts or pathlib.PurePosixPath(name).name in forbidden for name in files):
            fail('private pairing, settings, or log file included')
    print(f'PASS: {package.name} contains the {app["title"]} receiver and no private state')


if __name__ == '__main__':
    main()
