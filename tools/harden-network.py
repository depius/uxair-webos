#!/usr/bin/env python3
"""Bound production UxPlay allocations without changing codec parsing."""
import hashlib
from pathlib import Path
import sys

PINS = {
    'raop_rtp_mirror.c': '4ddedfdb16e7a35295bc5fd150a35284b15361c0',
    'http_request.c': 'ec030c4a1dd5f10597f44b003c2adf079288c1ea',
}
MIRROR_LIMIT = 2097152  # Existing largest receiver ingress limit, not a new profile.
HTTP_LIMIT = 33554432  # Existing development HTTP body ceiling.


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError('Pinned allocation site changed; refusing to patch')
    return source.replace(old, new, 1)


def bound_http_request(source, diagnostic=''):
    old = '    request->data = realloc(request->data, request->datalen + length);\n'
    new = (
        f'    if (request->datalen < 0 || length > {HTTP_LIMIT}U ||\n'
        f'        (size_t) request->datalen > {HTTP_LIMIT}U - length) {{\n'
        f'        {diagnostic}\n'
        '        return -1;\n'
        '    }\n'
        '    void *bounded_data = realloc(request->data, request->datalen + length);\n'
        '    if (!bounded_data && (request->datalen + length) != 0) return -1;\n'
        '    request->data = bounded_data;\n')
    return replace_once(source, old, new)


def bound_mirror(source):
    old = '            int payload_size = byteutils_get_int(packet, 0);\n'
    source = replace_once(source, old, old +
        f'            if (payload_size < 0 || payload_size > {MIRROR_LIMIT}) {{\n'
        '                logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "mirror allocation bound failed");\n'
        '                conn_reset = true; break;\n'
        '            }\n')
    source = replace_once(source, '                payload = malloc(payload_size);\n',
        '                payload = malloc(payload_size ? payload_size : 1);\n'
        '                if (!payload) { conn_reset = true; break; }\n')
    old = ('                    payload_out = (unsigned char*) malloc(payload_size + sps_pps_len);\n'
           '                    if (!payload_out) {\n'
           '                        printf("Memory allocation failed (payload_out)\\n");\n'
           '                        exit(1);\n'
           '                    }\n')
    source = replace_once(source, old,
        f'                    if (sps_pps_len < 0 || sps_pps_len > {MIRROR_LIMIT} - payload_size) {{\n'
        '                        conn_reset = true; break;\n'
        '                    }\n'
        '                    payload_out = (unsigned char*) malloc(payload_size + sps_pps_len);\n'
        '                    if (!payload_out) { conn_reset = true; break; }\n')
    source = replace_once(source,
        '                    payload_out = (unsigned char*)  malloc(payload_size);\n',
        '                    payload_out = (unsigned char*) malloc(payload_size ? payload_size : 1);\n'
        '                    if (!payload_out) { conn_reset = true; break; }\n')
    old = '                    sps_pps = (unsigned char*) malloc(sps_pps_len);\n'
    # H.264 and HEVC allocation sites share this statement. All must be guarded.
    if source.count(old) != 2:
        raise ValueError('Pinned parameter allocation sites changed')
    source = source.replace(old,
        f'                    if (sps_pps_len <= 0 || sps_pps_len > {MIRROR_LIMIT}) {{\n'
        '                        conn_reset = true; break;\n'
        '                    }\n' + old +
        '                    if (!sps_pps) { conn_reset = true; break; }\n')
    source = replace_once(source,
        '            if (unsupported_codec) {\n                break;\n            }\n',
        '            if (unsupported_codec || conn_reset) {\n                break;\n            }\n')
    return replace_once(source, '    /* Close the stream file descriptor */\n',
        '    free(payload);\n    free(sps_pps);\n'
        '    /* Close the stream file descriptor */\n')


def main():
    if len(sys.argv) != 3:
        raise SystemExit('Usage: harden-network.py PINNED_LIB_DIR OUTPUT_DIR')
    lib, out = map(Path, sys.argv[1:])
    original = {}
    for name, pin in PINS.items():
        data = (lib / name).read_bytes()
        digest = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if digest != pin:
            raise SystemExit('Unpinned network input: ' + name)
        original[name] = data.decode('utf-8')
    generated = {'raop_rtp_mirror.c': bound_mirror(original['raop_rtp_mirror.c']),
                 'http_request.c': bound_http_request(original['http_request.c'])}
    out.mkdir(parents=True, exist_ok=True)
    for name, source in generated.items():
        (out / ('bounded-' + name)).write_text(source, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
