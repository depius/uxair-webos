"""Replay clear Apple and Mux media through the real downloader and mock decoder."""
import functools
import http.server
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import threading
from urllib.parse import urljoin
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
APPLE = 'https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_4x3/'
MUX = 'https://test-streams.mux.dev/x36xhzz/'
TS_SIZE = 188


def download(url, limit):
    with urlopen(url, timeout=15) as source:
        data = source.read(limit + 1)
    assert data and len(data) <= limit, 'public fixture exceeds download bounds'
    return data


def fixture(root, upstream, name):
    manifest = download(urljoin(upstream, name), 256 * 1024).decode()
    directory = root / Path(name).parent
    directory.mkdir(parents=True, exist_ok=True)
    lines = []
    segments = []
    for line in manifest.splitlines():
        lines.append(line)
        if not line or line.startswith('#'):
            continue
        target = (directory / line).resolve()
        assert target.is_relative_to(directory.resolve())
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(download(urljoin(urljoin(upstream, name), line), 8 * 1024 * 1024))
        segments.append(line)
        if len(segments) == 2:
            break
    assert len(segments) == 2
    (root / name).write_text('\n'.join(lines + ['#EXT-X-ENDLIST', '']))
    return segments


def invalid_pts(segment):
    data = bytearray(segment)
    frames = 0
    for offset in range(0, len(data) - TS_SIZE + 1, TS_SIZE):
        assert data[offset] == 0x47
        if not (data[offset + 1] & 0x40 and data[offset + 3] & 0x10):
            continue
        payload = offset + 4
        if data[offset + 3] & 0x20:
            payload += 1 + data[payload]
        if payload + 19 > offset + TS_SIZE or data[payload:payload + 4] != b'\x00\x00\x01\xe0':
            continue
        if data[payload + 7] & 0xc0 != 0xc0:
            continue
        frames += 1
        if frames != 6:
            continue
        # Keep the MPEG PES marker bits and decode order, but inject an
        # impossible two-second composition offset into one video AU.
        i = payload + 14
        dts = (((data[i] >> 1) & 7) << 30 | data[i + 1] << 22 |
               (data[i + 2] >> 1) << 15 | data[i + 3] << 7 | data[i + 4] >> 1)
        pts = (dts + 180000) % (1 << 33)
        i = payload + 9
        data[i:i + 5] = bytes((0x31 | ((pts >> 29) & 14), (pts >> 22) & 255,
                               1 | ((pts >> 14) & 254), (pts >> 7) & 255, 1 | ((pts << 1) & 254)))
        return data
    raise AssertionError('fixture is missing a video PES with both timestamps')


class QuietServer(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass


with tempfile.TemporaryDirectory(prefix='uxair-public-hls-') as folder:
    root = Path(folder)
    apple = root / 'apple'
    mux = root / 'mux'
    apple.mkdir()
    mux.mkdir()
    (apple / 'master.m3u8').write_bytes(download(APPLE + 'bipbop_4x3_variant.m3u8', 256 * 1024))
    (mux / 'master.m3u8').write_bytes(download(MUX + 'x36xhzz.m3u8', 256 * 1024))
    gear2, gear2_next = fixture(apple, APPLE, 'gear2/prog_index.m3u8')
    fixture(apple, APPLE, 'gear4/prog_index.m3u8')
    fixture(mux, MUX, 'url_0/193039199_mp4_h264_aac_hd_7.m3u8')
    first = 'gear2/' + gear2
    (apple / 'bad-timestamp.ts').write_bytes(invalid_pts((apple / first).read_bytes()))
    (apple / 'container.bin').write_bytes(b'\x00\x00\x00\x18ftypisom\x00\x00\x02\x00isomiso2')
    for name, segments in {
        'bad-timestamp': '#EXTINF:10,\nbad-timestamp.ts\n',
        'discontinuous': f'#EXTINF:10,\n{first}\n#EXT-X-DISCONTINUITY\n#EXTINF:10,\ngear2/{gear2_next}\n',
        'timeline-reset': f'#EXTINF:10,\n{first}\n#EXTINF:10,\n{first}\n',
        'init-map': '#EXT-X-MAP:URI="container.bin"\n#EXTINF:10,\ncontainer.bin\n',
        'container': '#EXTINF:10,\ncontainer.bin\n',
    }.items():
        (apple / (name + '.m3u8')).write_text('#EXTM3U\n' + segments + '#EXT-X-ENDLIST\n')

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
        route.connect(('192.0.2.1', 9))
        ip = route.getsockname()[0]
    handler = functools.partial(QuietServer, directory=folder)
    with http.server.ThreadingHTTPServer((ip, 0), handler) as server:
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            checked = subprocess.run(['build/tests/direct-bufferstream-test',
                                      f'http://{ip}:{server.server_port}', '--public'],
                                     timeout=55, cwd=ROOT, env=dict(os.environ, UXAIR_DIRECT_LAN=ip),
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        finally:
            server.shutdown()
            thread.join()
    for line in checked.stdout.splitlines():
        if checked.returncode or '[mpegts @' not in line:
            print(line)
    checked.check_returncode()
    assert 'token=PRIVATE' not in checked.stdout and 'https://' not in checked.stdout and 'http://' not in checked.stdout
    assert re.search(r'container=mpegts probe-score=[1-9]\d* video-codec=27 audio-codec=86018', checked.stdout)
    assert re.search(r'aac-rate=22050 aac-channels=2 .*sps-pps=1 first-video-au=1 first-pcm-bytes=[1-9]\d* .*pts-before-dts=[1-9]\d*', checked.stdout)
    assert re.search(r'geometry=1280x720 aac-rate=44100 .*first-pcm-bytes=[1-9]\d* aac-without-pcm=[1-9]\d*', checked.stdout)
    assert re.search(r'container=mpegts .*first-idr=0 .*timestamps=valid', checked.stdout), 'continuity across an open GOP'
    assert 'stage=segment-demux error=video-timestamp' in checked.stdout
    assert 'timestamp-problem=composition-offset' in checked.stdout
    assert 'stage=playlist-parse error=declared-discontinuity' in checked.stdout
    assert 'stage=segment-timeline error=discontinuity' in checked.stdout
    assert 'stage=playlist-parse error=init-segment-required' in checked.stdout
    assert 'stage=segment-demux error=unsupported-container' in checked.stdout
