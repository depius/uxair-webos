#!/usr/bin/env python3
"""Controlled, credential-free native acceptance test; never modifies app state files."""
import argparse
import http.client
import math
import plistlib
import socket
import sys
import threading
import time
import urllib.parse
import uuid
from xml.parsers.expat import ExpatError

STAGES = set('idle queued dequeued relay acb-create acb-initialize construct media-id load await-load sink loaded-state window seek play await-playing playing paused video-info stopped'.split())
ERRORS = set('none busy relay-start payload acb-create acb-initialize construct media-id load-rejected native-error sink seek play-rejected load-timeout duration-timeout playing-timeout progress-timeout control-rejected'.split())
COUNTERS = ('uxairNativeResult', 'uxairCallbackCount', 'uxairLastCallback', 'uxairLastCallbackValue', 'uxairRelayRequests', 'uxairUpstreamFetches', 'uxairDownloadedBytes', 'uxairRelayFailures')

class ClientFailure(Exception):
    pass

def decode(payload):
    try:
        info = plistlib.loads(payload)
        if not isinstance(info, dict): raise ValueError()
        return info
    except (ValueError, TypeError, OverflowError, ExpatError, plistlib.InvalidFileException):
        raise ClientFailure('invalid playback feedback') from None

def safe_summary(info):
    """Print only known labels and bounded numbers; never server/native text."""
    out = {}
    for key in ('readyToPlay', 'position', 'duration', 'rate') + COUNTERS:
        value = info.get(key)
        if isinstance(value, (bool, int, float)) and abs(value) <= 2**63 and math.isfinite(value):
            out[key] = value
    for key, choices in (('uxairStage', STAGES), ('uxairError', ERRORS)):
        if isinstance(info.get(key), str) and info[key] in choices: out[key] = info[key]
    return out

def run(receiver_url, source, poll_interval=1, wait_seconds=20):
    receiver = urllib.parse.urlsplit(receiver_url)
    owner = str(uuid.uuid4())
    stop = threading.Event()
    reverse = None
    thread = None
    last_event = {'state': 'none'}
    last_feedback = {}
    played = False

    def command(method, path, body=b''):
        conn = http.client.HTTPConnection(receiver.hostname, receiver.port, timeout=5)
        try:
            conn.request(method, path, body, {'X-Apple-Session-ID': owner, 'Content-Type': 'text/parameters'})
            result = conn.getresponse()
            payload = result.read(65537)
            print(method, path.split('?')[0], result.status, flush=True)
            if len(payload) > 65536: raise ClientFailure('oversized control response')
            if path == '/playback-info':
                if payload:
                    last_feedback.clear(); last_feedback.update(safe_summary(decode(payload)))
                    print('Feedback:', last_feedback, flush=True)
                if result.status != 200: raise ClientFailure('playback-info HTTP ' + str(result.status))
                return last_feedback.copy()
            if result.status != 200: raise ClientFailure('control HTTP ' + str(result.status))
        finally:
            conn.close()

    def events(pending):
        reverse.settimeout(1)
        try:
            while not stop.is_set():
                # Consume already buffered events before waiting for another packet.
                while b'\r\n\r\n' in pending:
                    header, body = pending.split(b'\r\n\r\n', 1)
                    length = next((int(line.split(b':', 1)[1]) for line in header.split(b'\r\n') if line.lower().startswith(b'content-length:')), 0)
                    if length < 0 or length > 524288: raise ClientFailure('invalid event length')
                    if len(body) < length: break
                    event = decode(body[:length]); pending = body[length:]
                    state = event.get('state')
                    if state in ('loading', 'playing', 'paused', 'stopped'):
                        last_event['state'] = state
                        print('Event:', state, flush=True)
                    reverse.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n')
                if len(pending) > 532480: raise ClientFailure('oversized event buffer')
                try:
                    block = reverse.recv(4096)
                    if not block:
                        last_event['connection'] = 'closed'; break
                    pending += block
                except socket.timeout:
                    continue
        except (OSError, ValueError, ClientFailure):
            last_event['connection'] = 'invalid-or-closed'

    try:
        reverse = socket.create_connection((receiver.hostname, receiver.port), timeout=5)
        reverse.sendall((f'POST /reverse HTTP/1.1\r\nHost: {receiver.hostname}\r\nX-Apple-Session-ID: {owner}\r\nConnection: Upgrade\r\nUpgrade: PTTH/1.0\r\nContent-Length: 0\r\n\r\n').encode())
        data = b''
        while b'\r\n\r\n' not in data:
            block = reverse.recv(4096)
            if not block: raise ClientFailure('reverse connection closed')
            data += block
            if len(data) > 8192: raise ClientFailure('oversized reverse response')
        if not data.startswith(b'HTTP/1.1 101 '): raise ClientFailure('reverse upgrade failed')
        print('Reverse upgrade: 101', flush=True)
        thread = threading.Thread(target=events, args=(data.split(b'\r\n\r\n', 1)[1],), daemon=True)
        thread.start()
        played = True
        command('POST', '/play', f'Content-Location: {source}\r\nStart-Position: 0\r\n'.encode())
        deadline = time.monotonic() + wait_seconds
        while time.monotonic() < deadline:
            time.sleep(poll_interval)
            info = command('GET', '/playback-info')
            if info.get('readyToPlay') and info.get('rate') == 1: break
            if last_event.get('state') == 'stopped': raise ClientFailure('session stopped before native playback became ready')
            if 'connection' in last_event: raise ClientFailure('reverse connection ended before native playback became ready')
        else:
            raise ClientFailure('native playback did not become ready within the test deadline')
        time.sleep(poll_interval); command('GET', '/playback-info')
        command('POST', '/rate?value=0'); time.sleep(poll_interval); command('GET', '/playback-info')
        command('POST', '/rate?value=1'); time.sleep(poll_interval)
        command('POST', '/scrub?position=5'); time.sleep(poll_interval); command('GET', '/playback-info')
    except (OSError, http.client.HTTPException, ValueError, ClientFailure) as error:
        # Exception text from HTTP/socket libraries can include credentials or URLs.
        reason = str(error) if isinstance(error, ClientFailure) else 'network/protocol failure (' + type(error).__name__ + ')'
        print('FAILED:', reason, '| last event:', last_event.get('state'), '| last feedback:', last_feedback, flush=True)
        return 1
    finally:
        if played:
            try: command('POST', '/stop')
            except (OSError, http.client.HTTPException, ValueError, ClientFailure):
                print('Cleanup: stop was not acknowledged; close the development app if still active.', flush=True)
        stop.set()
        if reverse:
            try: reverse.shutdown(socket.SHUT_RDWR)
            except OSError: pass
            reverse.close()
        if thread: thread.join(timeout=2)
    print('Control checks passed. Report picture/audio and progress units; HTTP controls cannot verify either.')
    return 0

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--receiver', required=True, help='http://TV-IP:receiver-port')
    parser.add_argument('--source', required=True, help='fixture master URL (not printed)')
    args = parser.parse_args()
    try:
        receiver = urllib.parse.urlsplit(args.receiver)
        valid = receiver.scheme == 'http' and receiver.hostname and receiver.port and not receiver.username and not receiver.password
    except ValueError:
        valid = False
    if not valid: parser.error('receiver must be http://TV-IP:port')
    if any(c in args.source for c in '\r\n'): parser.error('invalid source')
    return run(args.receiver, args.source)

if __name__ == '__main__':
    sys.exit(main())
