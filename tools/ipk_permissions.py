#!/usr/bin/env python3
"""Normalize package-internal permissions; preserve webOS-managed containers.

The B7's installation container must remain writable for jailed state creation.
This does not secure that ancestor against another user able to rename children.
"""
import argparse
import copy
import gzip
import io
import pathlib
import tarfile


def ar_members(data):
    if not data.startswith(b'!<arch>\n'):
        raise ValueError('Not an IPK ar archive')
    offset, result = 8, []
    while offset < len(data):
        header = data[offset:offset+60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError('Malformed ar header')
        name = header[:16].decode('ascii').strip().rstrip('/')
        size = int(header[48:58])
        offset += 60
        if size < 0 or offset+size+size%2 > len(data):
            raise ValueError('Truncated ar member')
        result.append((name, header, data[offset:offset+size]))
        offset += size+size%2
    if [x[0] for x in result] != ['debian-binary', 'control.tar.gz', 'data.tar.gz'] or result[0][2] != b'2.0\n':
        raise ValueError('Unexpected IPK structure')
    return result


def desired_mode(member, archive, app_id):
    raw = member.name
    name = raw[2:] if raw.startswith('./') else raw
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or not path.parts:
        raise ValueError('Unsafe package path')
    if not member.isfile() and not member.isdir():
        raise ValueError('Links and special files are not allowed')
    app = 'usr/palm/applications/' + app_id
    managed = {'usr', 'usr/palm', 'usr/palm/applications', 'usr/palm/packages',
               app, 'usr/palm/packages/' + app_id}
    if member.isdir():
        # Do not chmod platform-managed containers as part of packaging.
        return member.mode & 0o777 if name in managed else 0o755
    return 0o755 if archive == 'data.tar.gz' and name in {app+'/uxair', app+'/lib/libstdc++.so.6'} else 0o644


def process(data, app_id, check=False):
    result = bytearray(b'!<arch>\n')
    for name, header, payload in ar_members(data):
        if name != 'debian-binary':
            plain = io.BytesIO()
            with tarfile.open(fileobj=io.BytesIO(payload), mode='r:gz') as src, tarfile.open(fileobj=plain, mode='w', format=tarfile.GNU_FORMAT) as dst:
                seen = set()
                for original in src:
                    member = copy.copy(original)
                    normalized = member.name.removeprefix('./').rstrip('/')
                    if normalized in seen:
                        raise ValueError('Duplicate package path')
                    seen.add(normalized)
                    mode = desired_mode(member, name, app_id)
                    if check and member.mode != mode:
                        raise ValueError('Unsafe package mode: ' + member.name)
                    member.mode = mode
                    dst.addfile(member, src.extractfile(original) if member.isfile() else None)
            payload = gzip.compress(plain.getvalue(), mtime=0)
        header = header[:48] + f'{len(payload):<10}'.encode('ascii') + header[58:]
        result.extend(header + payload + (b'\n' if len(payload)%2 else b''))
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=pathlib.Path)
    parser.add_argument('--app-id', required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    data = process(args.package.read_bytes(), args.app_id, args.check)
    if not args.check:
        args.package.write_bytes(data)
    print('PASS: package-internal permissions; managed containers retained')


if __name__ == '__main__':
    main()
