#!/usr/bin/env python3
"""Validate public-release metadata without requiring the webOS toolchain."""
import json
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = json.loads((ROOT / 'packaging/appinfo.json').read_text())
EXPECTED_ID = 'com.github.depius.uxair'
EXPECTED_NAME = 'UxAir'
EXPECTED_DESCRIPTION = 'AirPlay Receiver for webOS'

def fail(message):
    print(f'FAIL: {message}', file=sys.stderr)
    raise SystemExit(1)

def decode_rgba_png(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        fail('launcher asset is not a PNG')
    offset, width, height, depth, color_type = 8, None, None, None, None
    compressed = bytearray()
    while offset < len(data):
        length = struct.unpack('>I', data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        chunk = data[offset + 8:offset + 8 + length]
        offset += length + 12
        if kind == b'IHDR':
            width, height, depth, color_type = struct.unpack('>IIBB', chunk[:10])[:4]
        elif kind == b'IDAT':
            compressed.extend(chunk)
        elif kind == b'IEND':
            break
    if depth != 8 or color_type != 6:
        fail('launcher PNGs must use 8-bit RGBA channels')
    raw = zlib.decompress(compressed)
    stride = width * 4
    previous = bytearray(stride)
    pixels = []
    cursor = 0
    for _ in range(height):
        filter_type = raw[cursor]
        scan = bytearray(raw[cursor + 1:cursor + 1 + stride])
        cursor += stride + 1
        for i in range(stride):
            left = scan[i - 4] if i >= 4 else 0
            above = previous[i]
            upper_left = previous[i - 4] if i >= 4 else 0
            if filter_type == 1:
                scan[i] = (scan[i] + left) & 255
            elif filter_type == 2:
                scan[i] = (scan[i] + above) & 255
            elif filter_type == 3:
                scan[i] = (scan[i] + ((left + above) // 2)) & 255
            elif filter_type == 4:
                estimate = left + above - upper_left
                da, db, dc = abs(estimate - left), abs(estimate - above), abs(estimate - upper_left)
                predictor = left if da <= db and da <= dc else above if db <= dc else upper_left
                scan[i] = (scan[i] + predictor) & 255
            elif filter_type != 0:
                fail('unsupported PNG scanline filter')
        pixels.extend(tuple(scan[i:i + 4]) for i in range(0, stride, 4))
        previous = scan
    return width, height, pixels

def main():
    if APP['id'] != EXPECTED_ID:
        fail('unexpected application ID')
    if APP['title'] != EXPECTED_NAME or APP['vendor'] != EXPECTED_NAME:
        fail('title and vendor must use the public product name')
    if APP.get('appDescription') != EXPECTED_DESCRIPTION:
        fail('app description does not match the approved product description')
    if len(APP['appDescription']) > 60:
        fail('launcher subtitle exceeds the webOS metadata limit')
    if APP.get('iconColor') != '#1D2D47' or APP.get('largeIcon') != 'largeIcon.png':
        fail('launcher tile color or large icon metadata is missing')
    if APP['main'] != 'uxair':
        fail('unexpected executable name')
    version = APP['version']
    if version != '1.2.0' or not re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)', version):
        fail('release version must be 1.2.0 SemVer')
    changelog = (ROOT / 'CHANGELOG.md').read_text()
    if f'## [{version}]' not in changelog:
        fail('CHANGELOG does not contain the current version')
    for path in ['README.md', 'THIRD_PARTY.md', 'docs/RELEASING.md', 'docs/DEVELOPMENT.md', 'docs/ICON_ASSETS.md', 'docs/RELEASE_NOTES_1.0.0_UI.md', 'docs/RELEASE_NOTES_1.0.0_RC.md', 'docs/RELEASE_NOTES_1.0.0_RC2.md', 'docs/RELEASE_NOTES_1.0.0_RC3.md', 'docs/RELEASE_NOTES_1.0.0_RC4.md', 'docs/RELEASE_NOTES_1.0.0_RC5.md', 'docs/REPOSITORY_METADATA.md']:
        if not (ROOT / path).is_file():
            fail(f'missing required documentation: {path}')
    tracked_sources = list((ROOT / 'src').glob('*.cpp'))
    text = '\n'.join(p.read_text() for p in tracked_sources)
    if 'org.webosbrew.uxplaylg' in text or 'UxPlay LG' in text:
        fail('legacy identity remains in application source')
    if any(text not in (ROOT / 'src/main.cpp').read_text(encoding='utf-8') for text in ('Matej Skuhala (@depius)', 'Based on UxPlay by FDH2; uxplay-webOS by @danlflh')):
        fail('About page must identify the UxAir developer and preserve both upstream credits')
    for size, path in [(80, ROOT / 'packaging/icon.png'), (130, ROOT / 'packaging/largeIcon.png'), (256, ROOT / 'assets/uxair-icon-256.png'), (512, ROOT / 'assets/uxair-icon-512.png')]:
        if not path.is_file():
            fail(f'missing {size}px UxAir icon asset')
        data = path.read_bytes()
        width, height, pixels = decode_rgba_png(data)
        if (width, height) != (size, size):
            fail(f'invalid {size}px UxAir icon asset')
        if any(pixel[3] != 255 for pixel in pixels):
            fail(f'{size}px launcher asset must be opaque edge to edge')
        corners = (pixels[0], pixels[size - 1], pixels[-size], pixels[-1])
        if any(pixel != (29, 45, 71, 255) for pixel in corners):
            fail(f'{size}px launcher tile does not use the solid slate-blue background')
        if any(r >= 160 and g >= 80 and b <= 130 and r > g for r, g, b, _ in pixels):
            fail(f'{size}px launcher asset contains yellow/orange pixels')
    if not (ROOT / 'assets/uxair-icon.svg').is_file():
        fail('missing scalable UxAir icon artwork')
    funding = (ROOT / '.github/FUNDING.yml').read_text().strip()
    if funding != 'buy_me_a_coffee: depius':
        fail('GitHub funding must use the confirmed Buy Me a Coffee username')
    readme = (ROOT / 'README.md').read_text()
    if 'https://buymeacoffee.com/depius' not in readme or 'assets/buy-me-a-coffee.svg' not in readme:
        fail('README donation badge must link to the confirmed creator profile')
    if not (ROOT / 'assets/bmc-qr.bmp').is_file():
        fail('missing bundled donation QR bitmap')
    coffee = ROOT / 'assets/bmc-icon.bmp'
    if not coffee.is_file():
        fail('missing SDL-compatible Buy Me a Coffee icon')
    icon = coffee.read_bytes()
    if icon[:2] != b'BM' or int.from_bytes(icon[18:22], 'little') != 120 or int.from_bytes(icon[22:26], 'little', signed=True) != 120:
        fail('Buy Me a Coffee icon must be a square 120x120 SDL bitmap')
    if not (ROOT / 'assets/bmc-icon-source.png').is_file():
        fail('missing original supplied Buy Me a Coffee artwork')
    print(f'PASS: {EXPECTED_NAME} {version} metadata is consistent')

if __name__ == '__main__':
    main()
