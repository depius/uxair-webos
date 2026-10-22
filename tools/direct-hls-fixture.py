#!/usr/bin/env python3
"""Serve a bounded, locally generated, unprotected Safari/HLS acceptance fixture."""
import argparse
import functools
import http.server
import pathlib
import subprocess
import tempfile
import time
import threading
import urllib.parse


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bind', default='127.0.0.1', help='LAN IPv4 required for physical TV access')
    parser.add_argument('--port', type=int, default=8080)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='uxair-hls-') as folder:
        root = pathlib.Path(folder)
        subprocess.run(['ffmpeg', '-nostdin', '-loglevel', 'error', '-f', 'lavfi', '-i',
                        'testsrc2=size=1280x720:rate=30', '-f', 'lavfi', '-i',
                        'sine=frequency=440:sample_rate=44100', '-t', '12', '-c:v', 'libx264',
                        '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-b:v', '700k',
                        '-g', '60', '-c:a', 'aac', '-b:a', '96k', '-f', 'hls',
                        '-hls_time', '2', '-hls_list_size', '0', str(root / 'media.m3u8')],
                       check=True, timeout=60)
        # Progressive control uses the same compressed H.264/AAC: remux only.
        subprocess.run(['ffmpeg', '-nostdin', '-loglevel', 'error', '-i', str(root / 'media.m3u8'),
                        '-c', 'copy', '-movflags', '+faststart', str(root / 'probe.mp4')], check=True, timeout=30)
        (root / 'master.m3u8').write_text('#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=1280x720,CODECS="avc1.42c01f,mp4a.40.2"\nmedia.m3u8\n')
        (root / 'index.html').write_text('<!doctype html><meta name="viewport" content="width=device-width"><title>UxAir HLS fixture</title><h1>Unprotected H.264/AAC HLS</h1><video controls playsinline x-webkit-airplay="allow" width="100%" src="master.m3u8"></video>')
        class Handler(http.server.SimpleHTTPRequestHandler):
            records = 0
            records_lock = threading.Lock()
            def log_message(self, *unused):
                pass  # Never print URL query strings.
            def log_request(self, code='-', size='-'):
                path = urllib.parse.urlsplit(self.path).path
                role = 'master' if path == '/master.m3u8' else 'media' if path == '/media.m3u8' else 'segment' if path.endswith('.ts') else 'mp4' if path == '/probe.mp4' else 'other'
                with Handler.records_lock:
                    if Handler.records >= 192: return
                    Handler.records += 1
                method = self.command if self.command in ('GET', 'HEAD') else 'other'
                print(f'[fixture] request={method} role={role} http={int(code)}', flush=True)
            def do_GET(self):
                if self.path == '/redirect':
                    self.send_response(302)
                    self.send_header('Location', '/master.m3u8')
                    self.end_headers()
                    return
                if self.path == '/loop':
                    self.send_response(302); self.send_header('Location','/loop'); self.end_headers(); return
                if self.path == '/slow.m3u8':
                    time.sleep(13); self.path='/master.m3u8'
                if self.path == '/bad.m3u8':
                    data=b'not a playlist'; self.send_response(200); self.end_headers(); self.wfile.write(data); return
                if self.path == '/missing-segment.m3u8':
                    data=b'#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\nmissing.ts\n#EXT-X-ENDLIST\n'
                    self.send_response(200); self.end_headers(); self.wfile.write(data); return
                super().do_GET()
        handler = functools.partial(Handler, directory=folder)
        with http.server.ThreadingHTTPServer((args.bind, args.port), handler) as server:
            print('Serving 12-second HLS fixture; /redirect tests a relative redirect; /missing.ts returns 404.')
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass


if __name__ == '__main__':
    main()
