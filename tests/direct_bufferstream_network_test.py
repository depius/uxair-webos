"""Run BUFFERSTREAM against the same generated media fixture used on the B7."""
import http.client
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[1]
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
    route.connect(('192.0.2.1', 9))
    ip = route.getsockname()[0]
with socket.socket() as available:
    available.bind((ip, 0))
    port = available.getsockname()[1]

fixture = subprocess.Popen([sys.executable, str(root / 'tools/direct-hls-fixture.py'),
                            '--bind', ip, '--port', str(port)],
                           stdout=subprocess.DEVNULL)
try:
    deadline = time.monotonic() + 65
    while True:
        connection = http.client.HTTPConnection(ip, port, timeout=1)
        try:
            connection.request('HEAD', '/master.m3u8')
            assert connection.getresponse().status == 200
            break
        except OSError:
            assert fixture.poll() is None and time.monotonic() < deadline
            time.sleep(.1)
        finally:
            connection.close()
    subprocess.run(['build/tests/direct-bufferstream-test', f'http://{ip}:{port}'],
                   check=True, timeout=40, cwd=root, env=dict(os.environ, UXAIR_DIRECT_LAN=ip))
finally:
    fixture.terminate()
    fixture.wait(timeout=5)
