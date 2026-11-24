#!/usr/bin/env python3
import argparse
import json
import pathlib
import shutil
import subprocess
import tarfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SDK = ROOT / '.cache/arm-webos-linux-gnueabi_sdk-buildroot'

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--development', action='store_true', help='Package the separately identified development receiver')
    args = parser.parse_args()
    backend_marker = ROOT / 'build/media-backend.txt'
    backend = backend_marker.read_text().strip() if backend_marker.exists() else ''
    if backend not in ('ndl', 'starfish'):
        parser.error('missing or invalid media backend marker; rebuild before packaging')
    platform = {'ndl': 'webos5plus', 'starfish': 'webos3-4'}[backend]
    variant = 'development' if args.development else 'production'
    marker = ROOT / 'build/package-variant.txt'
    if marker.exists() and marker.read_text().strip() != variant:
        parser.error('package variant differs from the compiled receiver')
    stage = ROOT / 'build/app'
    # Staging is disposable; never package test sessions or local pairing keys.
    if stage.exists():
        shutil.rmtree(stage)
    (stage / 'lib').mkdir(parents=True)
    (stage / 'licenses').mkdir()
    app = json.loads((ROOT / 'packaging/appinfo.json').read_text())
    if args.development:
        app['id'] += '.dev'
        app['title'] += ' Dev'
        if backend == 'starfish':
            app['version'] = '1.4.0'
            app['title'] = 'UxAir 1.4.0-dev'
            app['appDescription'] = 'Experimental Direct AirPlay Video receiver'
    (stage / 'appinfo.json').write_text(json.dumps(app, indent=2) + '\n')
    source_commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    (stage / 'build-info.txt').write_text(f'version={app["version"]}\ncommit={source_commit}\n')
    shutil.copy2(ROOT / 'packaging/icon.png', stage / 'icon.png')
    shutil.copy2(ROOT / 'packaging/largeIcon.png', stage / 'largeIcon.png')
    shutil.copy2(ROOT / 'build/uxair', stage / 'uxair')
    shutil.copy2(ROOT / 'assets/bmc-qr.bmp', stage / 'bmc-qr.bmp')
    shutil.copy2(ROOT / 'assets/bmc-icon.bmp', stage / 'bmc-icon.bmp')
    shutil.copy2(SDK / 'arm-webos-linux-gnueabi/sysroot/usr/lib/libstdc++.so.6', stage / 'lib/libstdc++.so.6')
    for source, dest in [
        ('LICENSE', 'GPL-3.0.txt'),
        ('.cache/openssl-3.5.8/LICENSE.txt', 'OpenSSL.txt'),
        ('.cache/libplist-2.7.0/COPYING', 'libplist.txt'),
        ('.cache/ffmpeg-8.1.2/COPYING.LGPLv2.1', 'FFmpeg.txt'),
        ('vendor/uxplay/lib/llhttp/LICENSE-MIT', 'llhttp.txt'),
        ('THIRD_PARTY.md', 'THIRD_PARTY.md'),
        ('packaging/GCC-RUNTIME-EXCEPTION.txt', 'GCC-RUNTIME-EXCEPTION.txt'),
    ]:
        shutil.copy2(ROOT / source, stage / 'licenses' / dest)
    shutil.copy2(ROOT / 'dependencies.lock.json', stage / 'dependencies.lock.json')
    strip = SDK / 'bin/arm-webos-linux-gnueabi-strip'
    subprocess.run([str(strip), str(stage / 'uxair')], check=True)
    if args.development and backend == 'starfish':
        shutil.copy2(ROOT / 'build/direct-uri-probe', stage / 'direct-uri-probe')
        shutil.copy2(ROOT / 'build/libuxair-video-control.so', stage / 'lib/libuxair-video-control.so')
        go_root = pathlib.Path(subprocess.check_output(['go', 'env', 'GOROOT'], text=True).strip())
        for name in ('LICENSE', 'PATENTS'):
            shutil.copy2(go_root / name, stage / 'licenses' / f'Go-{name}.txt')
        with tarfile.open(ROOT / '.cache/video-control/upstream-video-control-source.tar') as archive:
            for notice in ('LICENSE', 'COPYING.GPL', 'THIRD_PARTY_NOTICES.md', 'SOURCE.md'):
                with archive.extractfile('third_party/airplay-control/' + notice) as src:
                    (stage / 'licenses' / ('AirPlay-Control-' + notice)).write_bytes(src.read())
        shutil.copy2(ROOT / 'assets/hls-ca.pem', stage / 'hls-ca.pem')
        shutil.copy2(ROOT / '.cache/curl-8.23.0/COPYING', stage / 'licenses/curl.txt')
        shutil.copy2(ROOT / 'packaging/Mozilla-MPL-2.0.txt', stage / 'licenses/Mozilla-MPL-2.0.txt')
        subprocess.run([str(strip), str(stage / 'direct-uri-probe')], check=True)
        subprocess.run(['python3', str(ROOT / 'tools/verify-elf.py'), str(stage), 'direct-uri-probe'], check=True)
    subprocess.run(['python3', str(ROOT / 'tools/verify-elf.py'), str(stage)], check=True)
    (ROOT / 'dist').mkdir(exist_ok=True)
    # one package per player: ndl for webOS 5 and later, starfish for webOS 3-4
    package = ROOT / 'dist' / f"{app['id']}_{app['version']}_arm-{platform}.ipk"
    if shutil.which('ares-package'):
        subprocess.run(['ares-package', str(stage), '-o', str(ROOT / 'dist')], check=True)
        (ROOT / 'dist' / f"{app['id']}_{app['version']}_arm.ipk").replace(package)
    else:
        # no webOS CLI (e.g. an offline Linux build host): same IPK layout
        subprocess.run(['python3', str(ROOT / 'tools/make-ipk.py'), str(stage), str(package)], check=True)
    if not package.is_file() or not package.stat().st_size:
        raise SystemExit(f'Expected nonempty package was not produced: {package}')
    subprocess.run(['python3', str(ROOT / 'tools/ipk_permissions.py'), str(package), '--app-id', app['id']], check=True)
    print(f'Packaged {package}')

if __name__ == '__main__':
    main()
