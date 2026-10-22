import functools
import http.server
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import time

# No packets are sent: select the runner's local route address for a trusted LAN fixture.
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.connect(("192.0.2.1", 9))
ip = sock.getsockname()[0]
sock.close()
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *unused): pass
    def do_GET(self):
        if self.path in ("/redirect", "/loop", "/denied"):
            self.send_response(302)
            self.send_header("Location", "/ok" if self.path == "/redirect" else "/loop" if self.path == "/loop" else "http://127.0.0.1:1/private")
            self.end_headers()
            return
        if self.path == "/missing.ts": self.send_error(404); return
        if self.path == "/slow": time.sleep(13)
        data = b"x" * 1024 if self.path == "/large" else b"#EXTM3U\n#EXT-X-ENDLIST\n"
        self.send_response(200); self.send_header("Content-Length", str(len(data))); self.end_headers()
        try: self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError): pass
with tempfile.TemporaryDirectory() as folder:
    key, cert = Path(folder)/"key.pem", Path(folder)/"cert.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(cert), "-subj", "/CN=invalid-fixture", "-days", "1"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    plain = http.server.ThreadingHTTPServer((ip, 0), Handler)
    secure = http.server.ThreadingHTTPServer((ip, 0), Handler)
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); tls.load_cert_chain(cert, key)
    secure.socket = tls.wrap_socket(secure.socket, server_side=True)
    for server in (plain, secure): threading.Thread(target=server.serve_forever, daemon=True).start()
    env = dict(os.environ, UXAIR_DIRECT_LAN=ip)
    try:
        subprocess.run(["build/tests/hls-network-test", f"http://{ip}:{plain.server_port}", f"https://{ip}:{secure.server_port}/ok"], check=True, timeout=25, env=env)
    finally:
        for server in (plain, secure): server.shutdown(); server.server_close()
