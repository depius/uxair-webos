#!/usr/bin/env python3
"""Check that the packaged IPK contains the expected app and no local state."""
import argparse
import io
import json
import pathlib
import re
import subprocess
import sys
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
        app['version'] = '1.1.0'
    stage = ROOT / 'build/app'
    staged = json.loads((stage / 'appinfo.json').read_text())
    if staged != app:
        fail('staged appinfo does not match requested package identity')
    backend_marker = ROOT / 'build/media-backend.txt'   # as tools/package.py names it
    backend = backend_marker.read_text().strip() if backend_marker.exists() else ''
    if backend not in ('ndl', 'starfish'):
        fail('missing or invalid media backend marker')
    platform = {'ndl': 'webos5plus', 'starfish': 'webos3-4'}[backend]
    package = ROOT / 'dist' / f"{app['id']}_{app['version']}_arm-{platform}.ipk"
    if not package.is_file():
        fail(f'missing {package.name}')
    subprocess.run([sys.executable, str(ROOT / 'tools/ipk_permissions.py'), str(package), '--app-id', app['id'], '--check'], check=True)
    members = ar_members(package)
    if members.get('debian-binary') != b'2.0\n':
        fail('missing Debian package format version')
    controls = [value for name, value in members.items() if name.startswith('control.tar')]
    if len(controls) != 1:
        fail('expected exactly one control archive')
    with tarfile.open(fileobj=io.BytesIO(controls[0]), mode='r:*') as archive:
        entries = [entry for entry in archive if entry.isfile() and entry.name.lstrip('./') == 'control']
        if len(entries) != 1:
            fail('expected exactly one control file')
        control = dict(line.split(':', 1) for line in archive.extractfile(entries[0]).read().decode().splitlines() if ':' in line)
        for key, expected in [('Package', app['id']), ('Version', app['version']), ('Architecture', 'arm')]:
            if control.get(key, '').strip() != expected:
                fail(f'wrong control {key}')
        print('Package control:', {key: value.strip() for key, value in control.items()})
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
        for binary in ('uxair', 'lib/libstdc++.so.6'):
            if content(binary) != (stage / binary).read_bytes():
                fail(f'packaged {binary} differs from the ELF-verified staging file')
        if content('dependencies.lock.json') != (ROOT / 'dependencies.lock.json').read_bytes():
            fail('packaged dependency manifest differs from pinned inputs')
        packageinfo = json.loads(content('packageinfo.json'))
        if any(packageinfo.get(key) != expected for key, expected in
               [('id', app['id']), ('app', app['id']), ('version', app['version'])]):
            fail('packageinfo identity/version mismatch')
        qr = content('bmc-qr.bmp')
        if qr != (ROOT / 'assets/bmc-qr.bmp').read_bytes() or qr[:2] != b'BM':
            fail('bundled donation QR differs from the committed local image')
        if int.from_bytes(qr[18:22], 'little') != 407 or int.from_bytes(qr[22:26], 'little', signed=True) != 407:
            fail('donation QR must be the expected 407x407 bitmap')
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
