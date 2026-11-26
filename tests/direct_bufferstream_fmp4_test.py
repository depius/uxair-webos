"""Generate clear HLS and exercise real HTTP, FFmpeg and simulated BUFFERSTREAM."""
from collections import Counter
import functools
import http.server
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import threading
import time
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
HEADER = '#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:2\n#EXT-X-PLAYLIST-TYPE:VOD\n'


def generate(directory, rate, seconds, video=True, split_tracks=False, silent_1080p=False, codec='avc'):
    directory.mkdir()
    audio = f'anullsrc=cl=stereo:r={rate}' if silent_1080p else f'sine=frequency=440:sample_rate={rate}'
    command = ['ffmpeg', '-nostdin', '-v', 'error', '-f', 'lavfi',
               '-i', audio, '-ac', '2', '-c:a', 'aac', '-profile:a', 'aac_low']
    if video:
        geometry = '160x96' if codec == 'av1' else '1920x1080' if silent_1080p else '640x480'
        command[4:4] = ['-f', 'lavfi', '-i', f'testsrc2=size={geometry}:rate=30']
        if codec in ('hevc', 'cra', 'main10'):
            open_gop = int(codec == 'cra')
            command += ['-c:v', 'libx265', '-preset', 'ultrafast', '-tag:v', 'hvc1',
                        '-x265-params', f'log-level=error:pools=none:frame-threads=1:keyint=60:min-keyint=60:scenecut=0:open-gop={open_gop}:bframes=2',
                        '-profile:v', 'main10' if codec == 'main10' else 'main',
                        '-pix_fmt', 'yuv420p10le' if codec == 'main10' else 'yuv420p']
        elif codec == 'av1':
            command += ['-c:v', 'libaom-av1', '-cpu-used', '8', '-threads:v', '2', '-g', '60', '-pix_fmt', 'yuv420p']
        else:
            command += ['-c:v', 'libx264', '-threads:v', '2', '-preset', 'veryfast', '-profile:v', 'high',
                        '-pix_fmt', 'yuv420p', '-bf', '2', '-g', '60', '-sc_threshold', '0']
    command += ['-t', str(seconds), '-f', 'hls', '-hls_time', '2', '-hls_list_size', '0',
                '-hls_segment_type', 'fmp4', '-hls_fmp4_init_filename', 'init.mp4',
                '-hls_segment_filename', 'part%d.m4s', 'media.m3u8']
    if split_tracks:
        command[-1:-1] = ['-hls_segment_options', 'movflags=+separate_moof']
    subprocess.run(command, cwd=directory, check=True, timeout=25)


def entry(path):
    return f'#EXTINF:2.000,\n{path}\n'


def init(path):
    return f'#EXT-X-MAP:URI="{path}"\n'


class FixtureServer(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path in ('/cookie-entry', '/cross-entry'):
            assert url.query == 'sig=PRIVATE'
            self.send_response(302)
            location = 'cookie-target?sig=PRIVATE' if url.path == '/cookie-entry' else self.server.cross
            self.send_header('Location', location)
            self.send_header('Set-Cookie', 'media-auth=SECRET; Path=/; HttpOnly')
            self.end_headers()
            return
        if url.path in ('/cookie-target', '/cross-target'):
            assert url.query == 'sig=PRIVATE'
            cookie = self.headers.get('Cookie', '')
            if url.path == '/cross-target':
                assert not cookie, 'media cookie escaped to another origin'
            elif cookie != 'media-auth=SECRET':
                self.send_error(403)
                return
            self.path = '/cookie-media.m3u8'
        if url.path in ('/expired', '/unauthorized'):
            self.send_error(403 if url.path == '/expired' else 401)
            return
        if url.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/clip/media.m3u8')
            self.end_headers()
            return
        if url.path == '/startup-align.m3u8':
            self.server.align_requests += 1
            time.sleep(.35)
            self.send_response(302)
            self.send_header('Location', '/clip/media.m3u8')
            self.end_headers()
            return
        if url.path == '/slow-switch.m3u8':
            self.server.slow_requests += 1
            time.sleep(6)
            return
        if url.path not in ('/ranges.bin', '/bad-range.bin'):
            super().do_GET()
            return
        assert url.query == 'token=PRIVATE'
        match = re.fullmatch(r'bytes=(\d+)-(\d+)', self.headers['Range'])
        assert match
        start, end = map(int, match.groups())
        assert 0 <= start <= end < len(self.server.packed)
        self.server.requests[url.path, start, end] += 1
        self.send_response(206)
        content_start = start + (url.path == '/bad-range.bin')
        self.send_header('Content-Range', f'bytes {content_start}-{end}/{len(self.server.packed)}')
        self.send_header('Content-Length', str(end + 1 - start))
        self.end_headers()
        try:
            self.wfile.write(self.server.packed[start:end + 1])
        except (BrokenPipeError, ConnectionResetError):
            pass


with tempfile.TemporaryDirectory(prefix='uxair-fmp4-') as folder:
    root = Path(folder)
    generate(root / 'clip', 22050, 8)
    generate(root / 'next', 44100, 4, split_tracks=True)
    generate(root / 'audio', 44100, 4, video=False)
    generate(root / 'silent', 44100, 4, silent_1080p=True)
    generate(root / 'hevc', 44100, 8, codec='hevc')
    generate(root / 'cra', 44100, 8, codec='cra')
    generate(root / 'main10', 44100, 2, codec='main10')
    generate(root / 'av1', 44100, 2, codec='av1')
    (root / 'silent-ts').mkdir()
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-i', '../silent/media.m3u8',
                    '-c', 'copy', '-f', 'hls', '-hls_time', '2', '-hls_list_size', '0', 'media.m3u8'],
                   cwd=root / 'silent-ts', check=True, timeout=15)
    clip = root / 'clip'

    def playlist(name, body):
        (root / (name + '.m3u8')).write_text(HEADER + body + '#EXT-X-ENDLIST\n')

    def variant(group, location):
        return ('#EXT-X-STREAM-INF:BANDWIDTH=850000,RESOLUTION=640x480,FRAME-RATE=30,'
                f'CODECS="avc1.64001f,mp4a.40.2",AUDIO="{group}"\n{location}\n')

    external = ('#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="external",NAME="English",'
                'DEFAULT=YES,AUTOSELECT=YES,URI="audio/media.m3u8"\n')
    (root / 'master.m3u8').write_text('#EXTM3U\n' + variant('external', 'should-not-fetch.m3u8') +
                                     variant('muxed', 'redirect') + external +
                                     '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="muxed",NAME="Embedded"\n')
    (root / 'separate.m3u8').write_text('#EXTM3U\n' + variant('external', 'clip/media.m3u8') + external)
    (root / 'cookie-media.m3u8').write_text((clip / 'media.m3u8').read_text().replace('"init.mp4"', '"clip/init.mp4"').replace('\npart', '\nclip/part'))
    def codec_variant(uri, codecs, rate, video_range='SDR'):
        return f'#EXT-X-STREAM-INF:BANDWIDTH={rate},CODECS="{codecs},mp4a.40.2",VIDEO-RANGE={video_range}\n{uri}\n'
    avc = 'avc1.64001f'
    hevc = 'hvc1.1.6.L93.B0'
    (root / 'codec-master.m3u8').write_text('#EXTM3U\n' + codec_variant('hevc/media.m3u8', hevc, 1200000) +
                                         codec_variant('av1/media.m3u8', 'av01.0.05M.08', 2000000) +
                                         codec_variant('main10/media.m3u8', 'hvc1.2.4.L93.B0', 2200000, 'PQ') +
                                         codec_variant('clip/media.m3u8', avc, 600000))
    (root / 'fallback-master.m3u8').write_text('#EXTM3U\n' + codec_variant('av1/media.m3u8', avc, 700000) +
                                            codec_variant('hevc/media.m3u8', hevc, 750000))

    prefix = b'unused!'
    init_bytes = (clip / 'init.mp4').read_bytes()
    packed = bytearray(prefix + init_bytes)
    requests = Counter({('/ranges.bin', len(prefix), len(packed) - 1): 1})
    ranged_init = f'#EXT-X-MAP:URI="ranges.bin?token=PRIVATE",BYTERANGE="{len(init_bytes)}@{len(prefix)}"\n'
    ranged = ranged_init
    for i in range(4):
        fragment = (clip / f'part{i}.m4s').read_bytes()
        offset = f'@{len(packed)}' if i == 0 else ''
        ranged += f'#EXTINF:2,\n#EXT-X-BYTERANGE:{len(fragment)}{offset}\nranges.bin?token=PRIVATE\n'
        requests['/ranges.bin', len(packed), len(packed) + len(fragment) - 1] = 1
        packed.extend(fragment)
    packed.extend(b'never requested')
    playlist('ranges', ranged)
    playlist('invalid-range', ranged_init + '#EXTINF:2,\n#EXT-X-BYTERANGE:100\nranges.bin?token=PRIVATE\n')
    playlist('invalid-map-range', ranged.replace(f'@{len(prefix)}"', '"', 1))
    playlist('bad-range-response', ranged.replace('ranges.bin', 'bad-range.bin'))
    playlist('ignored-range', ranged.replace('ranges.bin', 'ignored-range.bin'))
    (root / 'ignored-range.bin').write_bytes(packed)

    head = init('clip/init.mp4') + entry('clip/part0.m4s') + entry('clip/part1.m4s')
    playlist('boundary', head + '#EXT-X-DISCONTINUITY\n' + entry('clip/part0.m4s') + entry('clip/part1.m4s'))
    playlist('changed-map', head + init('next/init.mp4') + entry('next/part0.m4s') + entry('next/part1.m4s'))
    playlist('hevc-boundary', init('hevc/init.mp4') + entry('hevc/part0.m4s') + entry('hevc/part1.m4s') +
             '#EXT-X-DISCONTINUITY\n' + init('cra/init.mp4') + entry('cra/part0.m4s') + entry('cra/part1.m4s'))
    playlist('missing-map', entry('clip/part0.m4s'))
    playlist('missing-init', init('absent.mp4') + entry('clip/part0.m4s'))
    (root / 'bad.m4s').write_bytes(struct.pack('>I4s', 1000, b'moof'))
    playlist('malformed', init('clip/init.mp4') + entry('bad.m4s'))
    (root / 'encrypted.mp4').write_bytes(init_bytes + struct.pack('>I4s', 8, b'pssh'))
    playlist('encrypted', init('encrypted.mp4') + entry('clip/part0.m4s'))
    playlist('encrypted-playlist', '#EXT-X-KEY:METHOD=SAMPLE-AES,URI="never-fetched"\n' +
             init('clip/init.mp4') + entry('clip/part0.m4s'))
    (root / 'mpeg4.mp4').write_bytes(init_bytes.replace(b'avc1', b'mp4v'))
    playlist('unsupported', init('mpeg4.mp4') + entry('clip/part0.m4s'))
    playlist('audio-only', init('audio/init.mp4') + entry('audio/part0.m4s'))
    with (root / 'limit.m4s').open('wb') as target:
        target.truncate(8 * 1024 * 1024 + 1)
    playlist('oversized', init('clip/init.mp4') + entry('limit.m4s'))

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
        route.connect(('192.0.2.1', 9))
        ip = route.getsockname()[0]
    handler = functools.partial(FixtureServer, directory=root)
    with http.server.ThreadingHTTPServer((ip, 0), handler) as server, http.server.ThreadingHTTPServer((ip, 0), handler) as other:
        server.packed = packed
        server.cross = f'http://{ip}:{other.server_port}/cross-target?sig=PRIVATE'
        server.requests = Counter()
        server.slow_requests = 0
        server.align_requests = 0
        thread = threading.Thread(target=server.serve_forever)
        cross = threading.Thread(target=other.serve_forever)
        thread.start()
        cross.start()
        try:
            checked = subprocess.run(['build/tests/direct-bufferstream-test',
                                      f'http://{ip}:{server.server_port}', '--fmp4'],
                                     timeout=65, cwd=ROOT, env=dict(os.environ, UXAIR_DIRECT_LAN=ip),
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        finally:
            server.shutdown()
            other.shutdown()
            thread.join()
            cross.join()
        print(checked.stdout, end='')
        checked.check_returncode()
        assert {key: n for key, n in server.requests.items() if key[0] == '/ranges.bin'} == requests
        assert server.slow_requests == 1
        assert server.align_requests == 1
    for rate in (22050, 44100):
        assert re.search(rf'container=fmp4 .*aac-rate={rate} aac-channels=2 .*sps-pps=1 .*'
                         r'first-pcm-bytes=[1-9]\d* .*timestamps=valid .*tracks=2', checked.stdout)
    assert re.search(r'aac-rate=44100 .*timestamps=valid .*fragments=2', checked.stdout)
    assert 'stage=manifest-cancelled' in checked.stdout and 'stage=replace-completed' in checked.stdout
    assert re.search(r'video-codec-id=\d+ video-codec-name=av1', checked.stdout)
    assert re.search(r'video-codec-name=hevc hevc-rap=21', checked.stdout)
    assert 'stage=variant-fallback' in checked.stdout
    assert 'stage=manifest-fetch error=http-401' in checked.stdout and 'stage=manifest-fetch error=http-403' in checked.stdout
    assert 'PRIVATE' not in checked.stdout and 'SECRET' not in checked.stdout and 'http://' not in checked.stdout
