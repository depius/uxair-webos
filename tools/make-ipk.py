#!/usr/bin/env python3
"""Build a webOS IPK from a staged app folder without the webOS CLI.

Same layout as ares-package: an ar archive of debian-binary, control.tar.gz and
data.tar.gz, the app under usr/palm/applications/<id>/ and its packageinfo.json
under usr/palm/packages/<id>/.

    tools/make-ipk.py <staged app folder> <output .ipk>
"""
import io
import json
import pathlib
import sys
import tarfile
import time

def tar_gz(add):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:gz', format=tarfile.GNU_FORMAT) as tar:
        add(tar)
    return buf.getvalue()

def owned_by_root(info):
    info.uid = info.gid = 0
    info.uname = info.gname = 'root'
    return info

def add_bytes(tar, name, data, mode=0o644):
    info = owned_by_root(tarfile.TarInfo(name))
    info.size, info.mode, info.mtime = len(data), mode, int(time.time())
    tar.addfile(info, io.BytesIO(data))

def add_dir(tar, name):
    info = owned_by_root(tarfile.TarInfo(name))
    info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, int(time.time())
    tar.addfile(info)

def ar_member(name, data):
    header = f'{name + "/":<16}{int(time.time()):<12}{0:<6}{0:<6}{"100644":<8}{len(data):<10}`\n'
    return header.encode() + data + (b'\n' if len(data) % 2 else b'')

def main():
    stage, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    app = json.loads((stage / 'appinfo.json').read_text())
    app_id, version = app['id'], app['version']
    size_kb = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file()) // 1024 + 1

    def data(tar):
        for part in ('usr', 'usr/palm', 'usr/palm/applications', 'usr/palm/packages',
                     f'usr/palm/packages/{app_id}'):
            add_dir(tar, './' + part)
        tar.add(stage, arcname=f'./usr/palm/applications/{app_id}', filter=owned_by_root)
        info = {'app': app_id, 'id': app_id, 'loc_name': app.get('title', app_id),
                'package_format_version': 2, 'vendor': app.get('vendor', ''), 'version': version}
        add_bytes(tar, f'./usr/palm/packages/{app_id}/packageinfo.json',
                  (json.dumps(info, indent=2) + '\n').encode())

    control_text = (f'Package: {app_id}\nVersion: {version}\nSection: misc\nPriority: optional\n'
                    f'Architecture: arm\nInstalled-Size: {size_kb}\nMaintainer: N/A <nobody@example.com>\n'
                    f'Description: {app.get("title", app_id)}\nwebOS-Package-Format-Version: 2\n'
                    f'webOS-Packager-Version: make-ipk.py\n')
    control = tar_gz(lambda tar: add_bytes(tar, './control', control_text.encode()))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b'!<arch>\n' + ar_member('debian-binary', b'2.0\n')
                    + ar_member('control.tar.gz', control) + ar_member('data.tar.gz', tar_gz(data)))
    print(f'Wrote {out} ({out.stat().st_size // 1024} KB)')

if __name__ == '__main__':
    main()
