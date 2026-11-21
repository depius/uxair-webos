#!/usr/bin/env python3
"""Real local HTTP/PTTH failures produce bounded, credential-free client summaries."""
import http.server
import subprocess
import sys
import threading
from pathlib import Path
import plistlib

CLIENT = Path(__file__).resolve().parents[1] / 'tools/direct-control-test.py'
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *unused): pass
    def reply(self, status, data=b''):
        self.send_response(status); self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', 0)))
        if self.path == '/reverse':
            event = plistlib.dumps({'state': 'stopped'})
            self.send_response(101); self.send_header('Connection', 'Upgrade'); self.send_header('Upgrade', 'PTTH/1.0'); self.end_headers()
            # Coalesced upgrade/event exercises parsing of already buffered bytes.
            self.wfile.write(b'POST /event HTTP/1.1\r\nContent-Length: '+str(len(event)).encode()+b'\r\n\r\n'+event); self.wfile.flush()
            self.connection.settimeout(5)
            try: self.rfile.readline()
            except OSError: pass
            self.close_connection = True
        else:
            self.reply(500 if self.path == '/stop' else 200)
    def do_GET(self):
        if self.server.case == 'malformed': self.reply(200, b'not a plist SECRET-URL'); return
        if self.server.case == 'empty503': self.reply(503); return
        info = {'uxairStage': 'await-load', 'uxairError': 'load-timeout', 'uxairNativeResult': 1,
                'uxairCallbackCount': 0, 'uxairRelayRequests': 0, 'uxairUpstreamFetches': 0,
                'credential': 'https://PRIVATE.invalid/?token=SECRET', 'rate': 0, 'readyToPlay': False}
        self.reply(200 if self.server.case == 'stopped' else 503, plistlib.dumps(info))

for case in ('diagnostic503', 'empty503', 'malformed', 'stopped'):
    with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        server.case = case
        threading.Thread(target=server.serve_forever, daemon=True).start()
        result = subprocess.run([sys.executable, str(CLIENT), '--receiver', f'http://127.0.0.1:{server.server_port}',
                                 '--source', 'http://fixture.invalid/master.m3u8?token=SECRET'], capture_output=True, text=True, timeout=10)
        server.shutdown()
    assert result.returncode == 1, (case, result.stdout, result.stderr)
    assert 'FAILED:' in result.stdout and 'Cleanup: stop was not acknowledged' in result.stdout
    assert 'Traceback' not in result.stdout + result.stderr and 'SECRET' not in result.stdout + result.stderr and 'PRIVATE' not in result.stdout + result.stderr
    if case == 'diagnostic503': assert 'load-timeout' in result.stdout and 'playback-info HTTP 503' in result.stdout
    if case == 'stopped': assert 'stopped before native playback' in result.stdout
print('PASS: HTTP 503, error diagnostics, malformed feedback, buffered stopped event and failed cleanup without traceback/credentials')
